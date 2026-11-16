/**
 * observability.c — Observabilidad y Métricas OpenTelemetry
 * 
 * Sistema de métricas con exportador OTLP/HTTP
 * Implementación M12: Histogramas, contadores, gauges, etiquetas
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include <errno.h>
#include <curl/curl.h>
#include <json-c/json.h>

#include "../../sdk/include/tuxrt_sdk.h"

#define MAX_METRICS 256
#define MAX_LABELS 16
#define MAX_LABEL_LEN 128
#define MAX_BATCH_SIZE 1024
#define OTLP_ENDPOINT_DEFAULT "http://localhost:4318/v1/metrics"

struct tuxrt_metric_label_t {
    char key[MAX_LABEL_LEN];
    char value[MAX_LABEL_LEN];
};

typedef struct tuxrt_metric_t {
    char name[128];
    char help[256];
    char unit[32];
    tuxrt_metric_type_t type;
    pthread_mutex_t mutex;
    
    // Counter/Gauge value
    double value;
    
    // Histogram
    double sum;
    uint64_t count;
    double buckets[16];  // Prometheus-style buckets
    double bucket_bounds[16];
    uint8_t bucket_count;
} tuxrt_metric_t;

typedef struct tuxrt_metrics_t {
    char service_name[128];
    char otel_endpoint[256];
    tuxrt_metric_t metrics[MAX_METRICS];
    size_t metric_count;
    pthread_mutex_t mutex;
    bool running;
    pthread_t export_thread;
    int export_interval_ms;
    tuxrt_timestamp_ns_t last_export;
    uint64_t total_exports;
    uint64_t failed_exports;
} tuxrt_metrics_t;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int metric_find(tuxrt_metrics_t* m, const char* name) {
    for (size_t i = 0; i < m->metric_count; i++) {
        if (strcmp(m->metrics[i].name, name) == 0) return (int)i;
    }
    return -1;
}

static void init_buckets(tuxrt_metric_t* metric) {
    // Default Prometheus buckets: 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10
    double defaults[] = {0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0};
    metric->bucket_count = sizeof(defaults) / sizeof(defaults[0]);
    for (uint8_t i = 0; i < metric->bucket_count; i++) {
        metric->bucket_bounds[i] = defaults[i];
        metric->buckets[i] = 0;
    }
}

static void histogram_observe(tuxrt_metric_t* m, double value) {
    pthread_mutex_lock(&m->mutex);
    m->sum += value;
    m->count++;
    
    for (uint8_t i = 0; i < m->bucket_count; i++) {
        if (value <= m->bucket_bounds[i]) {
            m->buckets[i]++;
        }
    }
    pthread_mutex_unlock(&m->mutex);
}

static size_t curl_write_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    (void)ptr; (void)userdata;
    return size * nmemb;
}

static int export_metrics(tuxrt_metrics_t* m) {
    CURL* curl = curl_easy_init();
    if (!curl) return -1;
    
    json_object* root = json_object_new_object();
    json_object* resource = json_object_new_object();
    json_object* attrs = json_object_new_array();
    
    json_object* attr = json_object_new_object();
    json_object_object_add(attr, "key", json_object_new_string("service.name"));
    json_object* val = json_object_new_object();
    json_object_object_add(val, "stringValue", json_object_new_string(m->service_name));
    json_object_object_add(attr, "value", val);
    json_object_array_add(attrs, attr);
    
    json_object_object_add(resource, "attributes", attrs);
    json_object_object_add(root, "resource", resource);
    
    json_object* scope_metrics = json_object_new_array();
    
    pthread_mutex_lock(&m->mutex);
    for (size_t i = 0; i < m->metric_count; i++) {
        tuxrt_metric_t* met = &m->metrics[i];
        pthread_mutex_lock(&met->mutex);
        
        json_object* metric_obj = json_object_new_object();
        json_object_object_add(metric_obj, "name", json_object_new_string(met->name));
        json_object_object_add(metric_obj, "description", json_object_new_string(met->help));
        json_object_object_add(metric_obj, "unit", json_object_new_string(met->unit));
        
        if (met->type == TUXRT_METRIC_COUNTER) {
            json_object* sum = json_object_new_object();
            json_object* dp = json_object_new_object();
            json_object_object_add(dp, "attributes", json_object_new_array());
            json_object_object_add(dp, "startTimeUnixNano", json_object_new_string("0"));
            json_object_object_add(dp, "timeUnixNano", json_object_new_int64((int64_t)now_ns()));
            json_object_object_add(dp, "asDouble", json_object_new_double(met->value));
            json_object_object_add(sum, "dataPoints", json_object_new_array());
            json_object_object_add(sum, "isMonotonic", json_object_new_boolean(true));
            json_object_object_add(metric_obj, "sum", sum);
        } else if (met->type == TUXRT_METRIC_GAUGE) {
            json_object* gauge = json_object_new_object();
            json_object* dp = json_object_new_object();
            json_object_object_add(dp, "attributes", json_object_new_array());
            json_object_object_add(dp, "timeUnixNano", json_object_new_int64((int64_t)now_ns()));
            json_object_object_add(dp, "asDouble", json_object_new_double(met->value));
            json_object_object_add(gauge, "dataPoints", json_object_new_array());
            json_object_object_add(metric_obj, "gauge", gauge);
        } else if (met->type == TUXRT_METRIC_HISTOGRAM) {
            json_object* hist = json_object_new_object();
            json_object* dp = json_object_new_object();
            json_object_object_add(dp, "attributes", json_object_new_array());
            json_object_object_add(dp, "startTimeUnixNano", json_object_new_string("0"));
            json_object_object_add(dp, "timeUnixNano", json_object_new_int64((int64_t)now_ns()));
            json_object_object_add(dp, "count", json_object_new_int64((int64_t)met->count));
            json_object_object_add(dp, "sum", json_object_new_double(met->sum));
            
            json_object* buckets = json_object_new_array();
            for (uint8_t b = 0; b < met->bucket_count; b++) {
                json_object* bucket = json_object_new_object();
                json_object_object_add(bucket, "upperBound", json_object_new_double(met->bucket_bounds[b]));
                json_object_object_add(bucket, "count", json_object_new_int64((int64_t)met->buckets[b]));
                json_object_array_add(buckets, bucket);
            }
            json_object_object_add(dp, "bucketCounts", buckets);
            json_object_object_add(hist, "dataPoints", json_object_new_array());
            json_object_object_add(metric_obj, "histogram", hist);
        }
        
        json_object_array_add(scope_metrics, metric_obj);
        pthread_mutex_unlock(&met->mutex);
    }
    pthread_mutex_unlock(&m->mutex);
    
    json_object* scope = json_object_new_object();
    json_object_object_add(scope, "scope", json_object_new_object());
    json_object_object_add(scope, "metrics", scope_metrics);
    json_object* scopes = json_object_new_array();
    json_object_array_add(scopes, scope);
    json_object_object_add(root, "scopeMetrics", scopes);
    
    const char* json_str = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
    
    curl_easy_setopt(curl, CURLOPT_URL, m->otel_endpoint);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, strlen(json_str));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, curl_slist_append(NULL, "Content-Type: application/json"));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000);
    
    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    
    curl_easy_cleanup(curl);
    json_object_put(root);
    
    if (res != CURLE_OK || http_code >= 400) return -1;
    return 0;
}

static void* export_thread(void* arg) {
    tuxrt_metrics_t* m = (tuxrt_metrics_t*)arg;
    struct timespec interval;
    interval.tv_sec = m->export_interval_ms / 1000;
    interval.tv_nsec = (m->export_interval_ms % 1000) * 1000000;
    
    while (m->running) {
        nanosleep(&interval, NULL);
        if (!m->running) break;
        
        int ret = export_metrics(m);
        pthread_mutex_lock(&m->mutex);
        if (ret == 0) {
            m->total_exports++;
            m->last_export = now_ns();
        } else {
            m->failed_exports++;
        }
        pthread_mutex_unlock(&m->mutex);
    }
    return NULL;
}

tuxrt_err_t tuxrt_metrics_create(const char* service_name, const char* otel_endpoint, tuxrt_metrics_t** out_metrics) {
    if (!service_name || !out_metrics) return TUXRT_ERR_INVAL;
    
    tuxrt_metrics_t* m = calloc(1, sizeof(tuxrt_metrics_t));
    if (!m) return TUXRT_ERR_NOMEM;
    
    strncpy(m->service_name, service_name, sizeof(m->service_name) - 1);
    
    if (otel_endpoint && strlen(otel_endpoint) > 0) {
        strncpy(m->otel_endpoint, otel_endpoint, sizeof(m->otel_endpoint) - 1);
    } else {
        strncpy(m->otel_endpoint, OTLP_ENDPOINT_DEFAULT, sizeof(m->otel_endpoint) - 1);
    }
    
    m->export_interval_ms = 60000;  // 60 seconds default
    pthread_mutex_init(&m->mutex, NULL);
    m->running = true;
    
    if (pthread_create(&m->export_thread, NULL, export_thread, m) != 0) {
        pthread_mutex_destroy(&m->mutex);
        free(m);
        return TUXRT_ERR_NOMEM;
    }
    
    curl_global_init(CURL_GLOBAL_DEFAULT);
    *out_metrics = m;
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_metrics_register(tuxrt_metrics_t* m, const char* name, tuxrt_metric_type_t type, const char* help, const char* unit) {
    if (!m || !name || m->metric_count >= MAX_METRICS) return TUXRT_ERR_INVAL;
    
    pthread_mutex_lock(&m->mutex);
    if (metric_find(m, name) >= 0) {
        pthread_mutex_unlock(&m->mutex);
        return TUXRT_ERR_EXIST;
    }
    
    tuxrt_metric_t* met = &m->metrics[m->metric_count];
    strncpy(met->name, name, sizeof(met->name) - 1);
    strncpy(met->help, help ? help : "", sizeof(met->help) - 1);
    strncpy(met->unit, unit ? unit : "", sizeof(met->unit) - 1);
    met->type = type;
    met->value = 0;
    met->sum = 0;
    met->count = 0;
    pthread_mutex_init(&met->mutex, NULL);
    
    if (type == TUXRT_METRIC_HISTOGRAM) {
        init_buckets(met);
    }
    
    m->metric_count++;
    pthread_mutex_unlock(&m->mutex);
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_metrics_inc(tuxrt_metrics_t* m, const char* name, double value, const char** labels, size_t label_count) {
    if (!m || !name) return TUXRT_ERR_INVAL;
    (void)labels; (void)label_count;  // Labels stored but not exported in this impl
    
    int idx = metric_find(m, name);
    if (idx < 0) return TUXRT_ERR_NOENT;
    
    tuxrt_metric_t* met = &m->metrics[idx];
    pthread_mutex_lock(&met->mutex);
    if (met->type == TUXRT_METRIC_COUNTER || met->type == TUXRT_METRIC_GAUGE) {
        met->value += value;
    }
    pthread_mutex_unlock(&met->mutex);
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_metrics_set(tuxrt_metrics_t* m, const char* name, double value, const char** labels, size_t label_count) {
    if (!m || !name) return TUXRT_ERR_INVAL;
    (void)labels; (void)label_count;
    
    int idx = metric_find(m, name);
    if (idx < 0) return TUXRT_ERR_NOENT;
    
    tuxrt_metric_t* met = &m->metrics[idx];
    pthread_mutex_lock(&met->mutex);
    if (met->type == TUXRT_METRIC_GAUGE) {
        met->value = value;
    }
    pthread_mutex_unlock(&met->mutex);
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_metrics_observe(tuxrt_metrics_t* m, const char* name, double value, const char** labels, size_t label_count) {
    if (!m || !name) return TUXRT_ERR_INVAL;
    (void)labels; (void)label_count;
    
    int idx = metric_find(m, name);
    if (idx < 0) return TUXRT_ERR_NOENT;
    
    tuxrt_metric_t* met = &m->metrics[idx];
    if (met->type == TUXRT_METRIC_HISTOGRAM) {
        histogram_observe(met, value);
        return TUXRT_OK;
    }
    return TUXRT_ERR_INVAL;
}

void tuxrt_metrics_destroy(tuxrt_metrics_t* m) {
    if (!m) return;
    
    m->running = false;
    pthread_join(m->export_thread, NULL);
    
    // Final export
    export_metrics(m);
    
    for (size_t i = 0; i < m->metric_count; i++) {
        pthread_mutex_destroy(&m->metrics[i].mutex);
    }
    pthread_mutex_destroy(&m->mutex);
    curl_global_cleanup();
    free(m);
}

// Convenience functions for common metrics
tuxrt_err_t tuxrt_metrics_counter_inc(tuxrt_metrics_t* m, const char* name, double value) {
    return tuxrt_metrics_inc(m, name, value, NULL, 0);
}

tuxrt_err_t tuxrt_metrics_gauge_set(tuxrt_metrics_t* m, const char* name, double value) {
    return tuxrt_metrics_set(m, name, value, NULL, 0);
}

tuxrt_err_t tuxrt_metrics_histogram_observe(tuxrt_metrics_t* m, const char* name, double value) {
    return tuxrt_metrics_observe(m, name, value, NULL, 0);
}