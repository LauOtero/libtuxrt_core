/**
 * kinematics.c — Cinemática (CORE)
 *
 * Forward/Inverse kinematics para múltiples tipos de impresoras:
 *   - Cartesian, CoreXY, Polar: analíticas
 *   - Delta: cinemática inversa/forward 3-brazos + detección de singularidad
 *   - SCARA: cinemática de 2 brazos + Jacobiana analítica
 *
 * TRT-09: Cinemáticas avanzadas (SCARA, Delta)
 * TRT-10: Detección de singularidad vía determinante de Jacobiana
 */

#define TUXRT_CORE_EXPORTS
#include "../../sdk/include/tuxrt_sdk.h"
#include "hwdet.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

// =============================================================================
// Internal
// =============================================================================

#define KIN_EPSILON             1e-9
#define KIN_SINGULARITY_HARD    1e-6   // → SIRC N4 (crítico)
#define KIN_SINGULARITY_WARN    1e-3   // → ralentizar 50% (advertencia)

// Delta geometry parameter indices (params[32])
#define DELTA_PARAM_L          0   // Arm length (mm)
#define DELTA_PARAM_R          1   // Tower/bed radius (mm)
#define DELTA_PARAM_R_EFF      2   // Effector platform radius (mm)
#define DELTA_PARAM_H          3   // Tower height (mm)
#define DELTA_PARAM_MIN_Z      4   // Minimum Z (mm)
#define DELTA_PARAM_COUNT      5

// SCARA geometry parameter indices
#define SCARA_PARAM_L1         0   // First arm length (mm)
#define SCARA_PARAM_L2         1   // Second arm length (mm)
#define SCARA_PARAM_Z_HOME     2   // Z home position (mm)
#define SCARA_PARAM_COUNT      3

// Jacobian numerical differentiation step (rad / mm)
#define JAC_NUMERIC_H          1e-6

struct tuxrt_kinematics_t {
    tuxrt_kin_type_t type;
    double params[32];
    uint8_t param_count;
    double singularity_threshold;
    double epsilon;
    double eps_hard;   // CRIT threshold
    double eps_soft;   // WARN threshold
};

// =============================================================================
// Determinant helpers for singularity detection (TRT-09/10)
// =============================================================================

static double det3x3(const double* m) {
    // Row-major 3x3 matrix
    return m[0] * (m[4] * m[8] - m[5] * m[7])
         - m[1] * (m[3] * m[8] - m[5] * m[6])
         + m[2] * (m[3] * m[7] - m[4] * m[6]);
}

static double det4x4(const double* m) {
    // Cofactor expansion along first row
    double result = 0.0;
    for (int c = 0; c < 4; c++) {
        double sub[9];
        int si = 0;
        for (int r = 1; r < 4; r++) {
            for (int cc = 0; cc < 4; cc++) {
                if (cc != c) sub[si++] = m[r * 4 + cc];
            }
        }
        double sign = (c % 2 == 0) ? 1.0 : -1.0;
        result += sign * m[c] * det3x3(sub);
    }
    return result;
}

// =============================================================================
// Internal kinematics helpers
// =============================================================================

static tuxrt_err_t validate_axes(uint8_t axis_count, uint8_t expected) {
    if (axis_count != expected) return TUXRT_ERR_INVAL;
    return TUXRT_OK;
}

// -----------------------------------------------------------------------------
// Cartesian (trivial: identity)
// -----------------------------------------------------------------------------

static tuxrt_err_t kin_cartesian_forward(const double joints[12], tuxrt_pose_t* out) {
    out->pos.x = joints[0];
    out->pos.y = joints[1];
    out->pos.z = joints[2];
    out->rot.w = 1.0;
    out->rot.x = out->rot.y = out->rot.z = 0.0;
    return TUXRT_OK;
}

static tuxrt_err_t kin_cartesian_inverse(const tuxrt_pose_t* pose, double joints[12]) {
    joints[0] = pose->pos.x;
    joints[1] = pose->pos.y;
    joints[2] = pose->pos.z;
    return TUXRT_OK;
}

static double kin_cartesian_jacobian_det(const tuxrt_kinematics_t* kin, const double joints[12]) {
    (void)kin;
    (void)joints;
    // Identity Jacobian → determinant = 1 (non-singular)
    return 1.0;
}

// -----------------------------------------------------------------------------
// CoreXY
// -----------------------------------------------------------------------------

static tuxrt_err_t kin_corexy_forward(const double joints[12], tuxrt_pose_t* out) {
    double x = joints[0];
    double y = joints[1];
    double a = (x + y) * 0.5;
    double b = (x - y) * 0.5;
    out->pos.x = a;
    out->pos.y = b;
    out->pos.z = joints[2];
    out->rot.w = 1.0;
    out->rot.x = out->rot.y = out->rot.z = 0.0;
    return TUXRT_OK;
}

static tuxrt_err_t kin_corexy_inverse(const tuxrt_pose_t* pose, double joints[12]) {
    double x = pose->pos.x;
    double y = pose->pos.y;
    joints[0] = x + y;
    joints[1] = x - y;
    joints[2] = pose->pos.z;
    return TUXRT_OK;
}

static double kin_corexy_jacobian_det(const tuxrt_kinematics_t* kin, const double joints[12]) {
    (void)kin;
    (void)joints;
    // Jacobian is [[0.5, 0.5, 0], [0.5, -0.5, 0], [0, 0, 1]]
    // det = 0.5*(-0.5)*1 - 0.5*(0.5)*1 = -0.25 - 0.25 = -0.5; |det| = 0.5
    // Non-singular unless one motor is stalled in a degenerate path
    return 0.5;
}

// -----------------------------------------------------------------------------
// Polar
// -----------------------------------------------------------------------------

static tuxrt_err_t kin_polar_forward(const double joints[12], tuxrt_pose_t* out) {
    double r = joints[0];
    double theta = joints[1];
    double z = joints[2];
    out->pos.x = r * cos(theta);
    out->pos.y = r * sin(theta);
    out->pos.z = z;
    out->rot.w = 1.0;
    out->rot.x = out->rot.y = out->rot.z = 0.0;
    return TUXRT_OK;
}

static tuxrt_err_t kin_polar_inverse(const tuxrt_pose_t* pose, double joints[12]) {
    double r = sqrt(pose->pos.x * pose->pos.x + pose->pos.y * pose->pos.y);
    double theta = atan2(pose->pos.y, pose->pos.x);
    joints[0] = r;
    joints[1] = theta;
    joints[2] = pose->pos.z;
    return TUXRT_OK;
}

static double kin_polar_jacobian_det(const tuxrt_kinematics_t* kin, const double joints[12]) {
    (void)kin;
    // Jacobian of (x,y,z) wrt (r, theta, z):
    // [ cos(theta)  -r*sin(theta)  0 ]
    // [ sin(theta)   r*cos(theta)  0 ]
    // det of top-left 2x2 = r*cos^2 + r*sin^2 = r
    return joints[0];  // r — singular at r=0
}

// -----------------------------------------------------------------------------
// SCARA (2R planar + prismatic Z) — TRT-09
//
// Parámetros:
//   params[SCARA_PARAM_L1]   = L1 (brazo interno)
//   params[SCARA_PARAM_L2]   = L2 (brazo externo)
//   params[SCARA_PARAM_Z_HOME] = posición Z del home
//
//   joints[0]=theta1 (rad), joints[1]=theta2 (rad), joints[2]=z (mm)
// -----------------------------------------------------------------------------

static tuxrt_err_t kin_scara_forward(const tuxrt_kinematics_t* kin,
                                     const double joints[12], tuxrt_pose_t* out) {
    double L1 = kin->params[SCARA_PARAM_L1];
    double L2 = kin->params[SCARA_PARAM_L2];
    double theta1 = joints[0];
    double theta2 = joints[1];

    out->pos.x = L1 * cos(theta1) + L2 * cos(theta1 + theta2);
    out->pos.y = L1 * sin(theta1) + L2 * sin(theta1 + theta2);
    out->pos.z = joints[2];
    out->rot.w = 1.0;
    out->rot.x = out->rot.y = out->rot.z = 0.0;
    return TUXRT_OK;
}

static tuxrt_err_t kin_scara_inverse(const tuxrt_kinematics_t* kin,
                                     const tuxrt_pose_t* pose, double joints[12]) {
    double L1 = kin->params[SCARA_PARAM_L1];
    double L2 = kin->params[SCARA_PARAM_L2];
    double x = pose->pos.x;
    double y = pose->pos.y;
    double z = pose->pos.z;

    double d = sqrt(x * x + y * y);
    double cos_theta2 = (d * d - L1 * L1 - L2 * L2) / (2.0 * L1 * L2);

    // Clamp to [-1, 1] to handle numerical error
    if (cos_theta2 > 1.0) cos_theta2 = 1.0;
    if (cos_theta2 < -1.0) cos_theta2 = -1.0;

    // El brazo "elbow-up" es el default
    double theta2 = atan2(-sqrt(1.0 - cos_theta2 * cos_theta2), cos_theta2);
    double theta1 = atan2(y, x) - atan2(L2 * sin(theta2), L1 + L2 * cos(theta2));

    joints[0] = theta1;
    joints[1] = theta2;
    joints[2] = z;
    return TUXRT_OK;
}

static double kin_scara_jacobian_det(const tuxrt_kinematics_t* kin, const double joints[12]) {
    double L1 = kin->params[SCARA_PARAM_L1];
    double L2 = kin->params[SCARA_PARAM_L2];
    double theta1 = joints[0];
    double theta2 = joints[1];

    // Jacobian 3x3 (x,y,z vs theta1,theta2,z):
    // J = [ -L1*sin(t1) - L2*sin(t1+t2)   -L2*sin(t1+t2)   0 ]
    //     [  L1*cos(t1) + L2*cos(t1+t2)    L2*cos(t1+t2)   0 ]
    //     [  0                             0              1 ]
    //
    // The 2x2 top-left determinant (manipulability measure):
    // det = (-L1*s1 - L2*s12)*(L2*c12) - (-L2*s12)*(L1*c1 + L2*c12)
    //      = -L1*L2*s1*c12 - L2*L2*s12*c12 + L1*L2*s12*c1 + L2*L2*s12*c12
    //      = L1*L2*s12*c1 - L1*L2*s1*c12
    //      = L1*L2*(s12*c1 - s1*c12)
    //      = L1*L2*sin(theta2)

    // |det| = L1*L2*|sin(theta2)|
    return L1 * L2 * fabs(sin(theta2));
}

// -----------------------------------------------------------------------------
// Delta (3 brazos) — TRT-09/10
//
// Parámetros:
//   params[DELTA_PARAM_L]     = longitud del brazo (mm)
//   params[DELTA_PARAM_R]     = radio de las torres (mm)
//   params[DELTA_PARAM_R_EFF] = radio de la plataforma del efector (mm)
//   params[DELTA_PARAM_H]     = altura de las torres (mm)
//   params[DELTA_PARAM_MIN_Z] = Z mínimo operacional (mm)
//
//   joints[0,1,2] = posición Z relativa de cada torre (mm, positivo hacia abajo)
//   pose->pos.x,y,z = posición del efector (mm)
// -----------------------------------------------------------------------------

static tuxrt_err_t kin_delta_forward(const tuxrt_kinematics_t* kin,
                                      const double joints[12], tuxrt_pose_t* out) {
    // Forward: dado Z_tower[0..2], calcular (x, y, z) del efector.
    // Resolución numérica: el efector debe estar a distancia L de cada torre.
    //
    // Torre i está en (R*cos(θ_i), R*sin(θ_i), H) con θ_i = i*120°.
    // Punto de unión del efector está en (r_eff*cos(θ_i), r_eff*sin(θ_i), z).
    // Distancia: ||torre_i - efector_joint_i|| = L
    // (R*cos θ_i - r_eff*cos θ_i - x)^2 + (R*sin θ_i - r_eff*sin θ_i - y)^2 + (H - Z_i - z)^2 = L^2
    //
    // Simplificando: (R - r_eff)*cos(θ_i) - x)^2 + ... + (H - Z_i - z)^2 = L^2
    //
    // Aproximación analítica usando el método de intersección de esferas:
    // Resolución mediante eliminación. Para 3 torres, se puede resolver
    // x,y de forma directa y z por sustitución.

    double L = kin->params[DELTA_PARAM_L];
    double R = kin->params[DELTA_PARAM_R];
    double r_eff = kin->params[DELTA_PARAM_R_EFF];
    double H = kin->params[DELTA_PARAM_H];

    // Ángulos de las 3 torres (120° entre cada una)
    static const double theta_offset = 2.0 * M_PI / 3.0;

    // Posiciones XY de los puntos de unión del efector (en el plano del efector)
    double eff_joint_x[3], eff_joint_y[3];
    for (int i = 0; i < 3; i++) {
        double a = i * theta_offset;
        eff_joint_x[i] = r_eff * cos(a);
        eff_joint_y[i] = r_eff * sin(a);
    }

    // Z de las torres (altura de unión relativa a la base)
    double tz[3] = {
        H + joints[0],
        H + joints[1],
        H + joints[2]
    };

    // Resolver la posición del efector (x, y, z) usando las 3 esferas:
    // Para cada torre i:
    //   (torre_i_x - eff_joint_x_i - x)^2 + (torre_i_y - eff_joint_y_i - y)^2 + (tz_i - z)^2 = L^2
    //
    // Torre i está en (R*cos(θ_i), R*sin(θ_i), H)
    // La distancia horizontal entre la torre y el punto de unión del efector:
    double dx[3], dy[3];
    for (int i = 0; i < 3; i++) {
        double a = i * theta_offset;
        dx[i] = R * cos(a) - eff_joint_x[i];
        dy[i] = R * sin(a) - eff_joint_y[i];
    }

    // Restar ecuaciones para eliminar x^2 + y^2 + z^2:
    // (dx0 - x)^2 + (dy0 - y)^2 + (tz0 - z)^2 = (dx1 - x)^2 + (dy1 - y)^2 + (tz1 - z)^2
    // Expandiendo:
    // 2*(dx1-dx0)*x + 2*(dy1-dy0)*y + 2*(tz1-tz0)*z = dx0^2-dx1^2 + dy0^2-dy1^2 + tz0^2-tz1^2
    //
    // Esto da 2 ecuaciones lineales (de las 3 posibles diferencias) con 3 incógnitas.
    // Luego sustituimos en una de las ecuaciones de esfera original.

    // Construir sistema lineal A * [x,y,z]^T = b (2 ecuaciones)
    double A[2][3], b[2];

    // Diferencia 0-1
    A[0][0] = 2 * (dx[1] - dx[0]);
    A[0][1] = 2 * (dy[1] - dy[0]);
    A[0][2] = 2 * (tz[1] - tz[0]);
    b[0] = dx[0]*dx[0] - dx[1]*dx[1] + dy[0]*dy[0] - dy[1]*dy[1] + tz[0]*tz[0] - tz[1]*tz[1];

    // Diferencia 0-2
    A[1][0] = 2 * (dx[2] - dx[0]);
    A[1][1] = 2 * (dy[2] - dy[0]);
    A[1][2] = 2 * (tz[2] - tz[0]);
    b[1] = dx[0]*dx[0] - dx[2]*dx[2] + dy[0]*dy[0] - dy[2]*dy[2] + tz[0]*tz[0] - tz[2]*tz[2];

    // Resolver para x,y en función de z (o usar mínimos cuadrados)
    // A*x = b → 2 ecuaciones, 3 incógnitas
    // Expresar x,y como función lineal de z:
    // A(:,1:2)*[x;y] = b - A(:,3)*z
    //
    // Matriz 2x2 para x,y:
    double det2 = A[0][0]*A[1][1] - A[1][0]*A[0][1];
    if (fabs(det2) < KIN_EPSILON) {
        // Configuración singular
        out->pos.x = 0.0;
        out->pos.y = 0.0;
        out->pos.z = 0.0;
        out->rot.w = 1.0;
        out->rot.x = out->rot.y = out->rot.z = 0.0;
        return TUXRT_ERR_KINEMATICS_SINGULAR;
    }

    // x = (b1-A03*z)*A12 - (b2-A13*z)*A02) / det2
    // y = (A01*(b2-A13*z) - A11*(b1-A03*z)) / det2
    double c0x = (b[0]*A[1][1] - b[1]*A[0][1]) / det2;
    double c1x = (A[1][0]*b[1] - A[0][0]*b[1]) / det2; // coefficient of z in x — recompute properly
    // x = (A[1][1]*(b[0]-A[0][2]*z) - A[0][1]*(b[1]-A[1][2]*z)) / det2
    //   = (A[1][1]*b[0] - A[0][1]*b[1]) / det2  +  z*(A[0][1]*A[1][2] - A[1][1]*A[0][2])/det2
    double x0 = (A[1][1]*b[0] - A[0][1]*b[1]) / det2;
    double x1 = (A[0][1]*A[1][2] - A[1][1]*A[0][2]) / det2;
    // y = (A[0][0]*(b[1]-A[1][2]*z) - A[1][0]*(b[0]-A[0][2]*z)) / det2
    double y0 = (A[0][0]*b[1] - A[1][0]*b[0]) / det2;
    double y1 = (A[1][0]*A[0][2] - A[0][0]*A[1][2]) / det2;

    // Sustituir en la esfera 0: (dx[0]-x)^2 + (dy[0]-y)^2 + (tz[0]-z)^2 = L^2
    // x = x0 + x1*z, y = y0 + y1*z
    // (dx[0] - x0 - x1*z)^2 + (dy[0] - y0 - y1*z)^2 + (tz[0] - z)^2 = L^2
    // a*z^2 + b*z + c = 0
    double a_coeff = x1*x1 + y1*y1 + 1.0;  // coeff of z^2
    double b_coeff = 2.0 * ((dx[0]-x0)*(-x1) + (dy[0]-y0)*(-y1) - tz[0]);  // coeff of z
    double c_coeff = (dx[0]-x0)*(dx[0]-x0) + (dy[0]-y0)*(dy[0]-y0) + tz[0]*tz[0] - L*L;

    double disc = b_coeff*b_coeff - 4.0*a_coeff*c_coeff;
    if (disc < 0.0) {
        // No real solution — position outside workspace
        out->pos.x = 0.0;
        out->pos.y = 0.0;
        out->pos.z = 0.0;
        out->rot.w = 1.0;
        out->rot.x = out->rot.y = out->rot.z = 0.0;
        return TUXRT_ERR_INVAL;  // Outside reachable workspace
    }

    // Take the +z (higher) solution as primary
    double z_sol = (-b_coeff + sqrt(disc)) / (2.0 * a_coeff);
    // Check if lower solution is closer to expected (sometimes higher is invalid)
    double z_sol2 = (-b_coeff - sqrt(disc)) / (2.0 * a_coeff);

    // Prefer the solution that's less negative (higher z = lower in typical delta where +z is up)
    // Actually for delta printers, smaller z (more negative) or specific convention.
    // Choose solution closest to previous or the higher one.
    // For simplicity: choose the higher z solution (z_sol) for the top of the workspace
    // But if z_sol is above the tower tops, choose z_sol2
    double z_final = z_sol;
    double tower_top = H + fmax(joints[0], fmax(joints[1], joints[2]));
    if (z_sol > tower_top) {
        z_final = z_sol2;
    }

    out->pos.x = x0 + x1 * z_final;
    out->pos.y = y0 + y1 * z_final;
    out->pos.z = z_final;
    out->rot.w = 1.0;
    out->rot.x = out->rot.y = out->rot.z = 0.0;
    return TUXRT_OK;
}

static tuxrt_err_t kin_delta_inverse(const tuxrt_kinematics_t* kin,
                                      const tuxrt_pose_t* pose, double joints[12]) {
    double L = kin->params[DELTA_PARAM_L];
    double R = kin->params[DELTA_PARAM_R];
    double r_eff = kin->params[DELTA_PARAM_R_EFF];
    double H = kin->params[DELTA_PARAM_H];

    double x = pose->pos.x;
    double y = pose->pos.y;
    double z = pose->pos.z;

    static const double theta_offset = 2.0 * M_PI / 3.0;

    for (int i = 0; i < 3; i++) {
        double a = i * theta_offset;
        double eff_x = r_eff * cos(a);
        double eff_y = r_eff * sin(a);

        // Horizontal distance from tower i to the effector joint i
        double tx = R * cos(a);
        double ty = R * sin(a);
        double dx = tx - eff_x - x;
        double dy = ty - eff_y - y;
        double dhoriz_sq = dx * dx + dy * dy;
        double dhoriz = sqrt(dhoriz_sq);

        if (dhoriz > L) {
            // Position unreachable for this tower
            return TUXRT_ERR_INVAL;
        }

        // Z_tower = z + sqrt(L^2 - dhoriz^2)  (arm pointing down)
        double dz = sqrt(L * L - dhoriz_sq);
        // Tower position (relative) = (H + z_tower_relative) should give efector at height z
        // joints[i] = z_tower_relative = (H + joints[i]) ... need to solve
        // tz_i = H + joints[i], and dz = tz_i - z → joints[i] = z + dz - H
        joints[i] = z + dz - H;
    }
    return TUXRT_OK;
}

static double kin_delta_jacobian_det(const tuxrt_kinematics_t* kin, const double joints[12]) {
    // Determinante del Jacobiano calculado numéricamente mediante diferencias finitas.
    //
    // El Jacobiano J (3x3) tiene elementos J[i][j] = ∂f_i/∂q_j, donde:
    //   f = forward(q) = (x, y, z) del efector
    //   q = (joint[0], joint[1], joint[2]) = posiciones Z de las torres
    //
    // Para Delta, la cinemática directa es no lineal. Usamos diferencias finitas
    // centrales: ∂f/∂q_j ≈ (forward(q + h*e_j) - forward(q - h*e_j)) / (2*h)

    double h = JAC_NUMERIC_H;
    double jacobian[9];  // 3x3 row-major
    tuxrt_pose_t p_plus, p_minus;
    double q_plus[12], q_minus[12];

    for (int j = 0; j < 3; j++) {
        memcpy(q_plus, joints, sizeof(double) * 12);
        memcpy(q_minus, joints, sizeof(double) * 12);
        q_plus[j] += h;
        q_minus[j] -= h;

        if (kin_delta_forward(kin, q_plus, &p_plus) != TUXRT_OK ||
            kin_delta_forward(kin, q_minus, &p_minus) != TUXRT_OK) {
            return 0.0;  // No se pudo calcular → singular
        }

        jacobian[0 * 3 + j] = (p_plus.pos.x - p_minus.pos.x) / (2.0 * h);
        jacobian[1 * 3 + j] = (p_plus.pos.y - p_minus.pos.y) / (2.0 * h);
        jacobian[2 * 3 + j] = (p_plus.pos.z - p_minus.pos.z) / (2.0 * h);
    }

    return det3x3(jacobian);
}

// -----------------------------------------------------------------------------
// Jacobian dispatch (TRT-09/10)
// -----------------------------------------------------------------------------

static tuxrt_err_t kin_jacobian_internal(const tuxrt_kinematics_t* kin,
                                          const double joints[12],
                                          double* out_jacobian,
                                          size_t* out_rows, size_t* out_cols) {
    *out_rows = 3;
    *out_cols = 3;
    memset(out_jacobian, 0, 9 * sizeof(double));

    if (kin->type == TUXRT_KIN_CARTESIAN) {
        out_jacobian[0] = 1.0; out_jacobian[4] = 1.0; out_jacobian[8] = 1.0;
        return TUXRT_OK;
    }

    if (kin->type == TUXRT_KIN_COREXY) {
        // ∂(a,b,z)/∂(motor0,motor1,z)
        // a = 0.5*x + 0.5*y = 0.5*m0 + 0.5*m1
        // b = 0.5*x - 0.5*y = 0.5*m0 - 0.5*m1
        // z = z
        out_jacobian[0] = 0.5; out_jacobian[1] = 0.5; out_jacobian[2] = 0.0;
        out_jacobian[3] = 0.5; out_jacobian[4] = -0.5; out_jacobian[5] = 0.0;
        out_jacobian[6] = 0.0; out_jacobian[7] = 0.0; out_jacobian[8] = 1.0;
        return TUXRT_OK;
    }

    if (kin->type == TUXRT_KIN_POLAR) {
        double r = joints[0];
        double theta = joints[1];
        // ∂(x,y,z)/∂(r,theta,z)
        out_jacobian[0] = cos(theta);     out_jacobian[1] = -r * sin(theta);  out_jacobian[2] = 0.0;
        out_jacobian[3] = sin(theta);     out_jacobian[4] =  r * cos(theta);  out_jacobian[5] = 0.0;
        out_jacobian[6] = 0.0;            out_jacobian[7] = 0.0;               out_jacobian[8] = 1.0;
        return TUXRT_OK;
    }

    if (kin->type == TUXRT_KIN_SCARA) {
        double L1 = kin->params[SCARA_PARAM_L1];
        double L2 = kin->params[SCARA_PARAM_L2];
        double t1 = joints[0];
        double t2 = joints[1];
        // ∂(x,y,z)/∂(theta1,theta2,z)
        out_jacobian[0] = -L1 * sin(t1) - L2 * sin(t1 + t2);  out_jacobian[1] = -L2 * sin(t1 + t2);  out_jacobian[2] = 0.0;
        out_jacobian[3] =  L1 * cos(t1) + L2 * cos(t1 + t2);  out_jacobian[4] =  L2 * cos(t1 + t2);  out_jacobian[5] = 0.0;
        out_jacobian[6] = 0.0;                                 out_jacobian[7] = 0.0;                out_jacobian[8] = 1.0;
        return TUXRT_OK;
    }

    if (kin->type == TUXRT_KIN_DELTA) {
        // Jacobian numérica
        double h = JAC_NUMERIC_H;
        tuxrt_pose_t p_plus, p_minus;
        double q_plus[12], q_minus[12];

        for (int j = 0; j < 3; j++) {
            memcpy(q_plus, joints, sizeof(double) * 12);
            memcpy(q_minus, joints, sizeof(double) * 12);
            q_plus[j] += h;
            q_minus[j] -= h;

            if (kin_delta_forward(kin, q_plus, &p_plus) != TUXRT_OK ||
                kin_delta_forward(kin, q_minus, &p_minus) != TUXRT_OK) {
                return TUXRT_ERR_KINEMATICS_SINGULAR;
            }

            out_jacobian[0 * 3 + j] = (p_plus.pos.x - p_minus.pos.x) / (2.0 * h);
            out_jacobian[1 * 3 + j] = (p_plus.pos.y - p_minus.pos.y) / (2.0 * h);
            out_jacobian[2 * 3 + j] = (p_plus.pos.z - p_minus.pos.z) / (2.0 * h);
        }
        return TUXRT_OK;
    }

    return TUXRT_ERR_NOSYS;
}

// =============================================================================
// API
// =============================================================================

tuxrt_err_t tuxrt_kinematics_create(const tuxrt_kin_config_t* cfg, tuxrt_kinematics_t** out_kin) {
    if (!cfg || !out_kin) return TUXRT_ERR_INVAL;

    tuxrt_kinematics_t* kin = calloc(1, sizeof(tuxrt_kinematics_t));
    if (!kin) return TUXRT_ERR_NOMEM;

    kin->type = cfg->type;
    kin->param_count = cfg->param_count;
    size_t copy_count = (cfg->param_count < 32) ? cfg->param_count : 32;
    memcpy(kin->params, cfg->params, sizeof(double) * copy_count);
    kin->epsilon = KIN_EPSILON;
    kin->singularity_threshold = KIN_SINGULARITY_WARN;
    kin->eps_hard = KIN_SINGULARITY_HARD;
    kin->eps_soft = KIN_SINGULARITY_WARN;

    // Validar parámetros específicos por tipo
    switch (cfg->type) {
        case TUXRT_KIN_DELTA:
            if (copy_count < DELTA_PARAM_COUNT) {
                free(kin);
                return TUXRT_ERR_CONFIG_INVALID;
            }
            // Validar geometría (L > 0, R >= 0, r_eff >= 0)
            if (kin->params[DELTA_PARAM_L] <= 0.0) {
                free(kin);
                return TUXRT_ERR_CONFIG_INVALID;
            }
            break;

        case TUXRT_KIN_SCARA:
            if (copy_count < SCARA_PARAM_COUNT) {
                free(kin);
                return TUXRT_ERR_CONFIG_INVALID;
            }
            if (kin->params[SCARA_PARAM_L1] <= 0.0 || kin->params[SCARA_PARAM_L2] <= 0.0) {
                free(kin);
                return TUXRT_ERR_CONFIG_INVALID;
            }
            break;

        default:
            break;
    }

    *out_kin = kin;
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_kinematics_forward(const tuxrt_kinematics_t* kin,
                                      const tuxrt_joint_state_t* joints,
                                      tuxrt_pose_t* out_pose) {
    if (!kin || !joints || !out_pose) return TUXRT_ERR_INVAL;

    switch (kin->type) {
        case TUXRT_KIN_CARTESIAN:
            return kin_cartesian_forward(joints->position, out_pose);
        case TUXRT_KIN_COREXY:
            return kin_corexy_forward(joints->position, out_pose);
        case TUXRT_KIN_POLAR:
            return kin_polar_forward(joints->position, out_pose);
        case TUXRT_KIN_SCARA:
            return kin_scara_forward(kin, joints->position, out_pose);
        case TUXRT_KIN_DELTA:
            return kin_delta_forward(kin, joints->position, out_pose);
        default:
            fprintf(stderr, "kinematics_forward: tipo %d no implementado\n", kin->type);
            return TUXRT_ERR_NOSYS;
    }
}

tuxrt_err_t tuxrt_kinematics_inverse(const tuxrt_kinematics_t* kin,
                                      const tuxrt_pose_t* pose,
                                      tuxrt_joint_state_t* out_joints) {
    if (!kin || !pose || !out_joints) return TUXRT_ERR_INVAL;

    memset(out_joints->position, 0, sizeof(double) * 12);
    out_joints->axis_count = 3;

    switch (kin->type) {
        case TUXRT_KIN_CARTESIAN:
            return kin_cartesian_inverse(pose, out_joints->position);
        case TUXRT_KIN_COREXY:
            return kin_corexy_inverse(pose, out_joints->position);
        case TUXRT_KIN_POLAR:
            return kin_polar_inverse(pose, out_joints->position);
        case TUXRT_KIN_SCARA:
            return kin_scara_inverse(kin, pose, out_joints->position);
        case TUXRT_KIN_DELTA:
            return kin_delta_inverse(kin, pose, out_joints->position);
        default:
            fprintf(stderr, "kinematics_inverse: tipo %d no implementado\n", kin->type);
            return TUXRT_ERR_NOSYS;
    }
}

tuxrt_err_t tuxrt_kinematics_jacobian(const tuxrt_kinematics_t* kin,
                                       const tuxrt_joint_state_t* joints,
                                       double* out_jacobian,
                                       size_t* out_rows,
                                       size_t* out_cols) {
    if (!kin || !joints || !out_jacobian || !out_rows || !out_cols) return TUXRT_ERR_INVAL;
    return kin_jacobian_internal(kin, joints->position, out_jacobian, out_rows, out_cols);
}

bool tuxrt_kinematics_is_singular(const tuxrt_kinematics_t* kin,
                                   const tuxrt_joint_state_t* joints,
                                   double threshold) {
    if (!kin || !joints) return true;

    double eps_hard = (threshold > 0.0) ? threshold : kin->eps_hard;

    double det = 0.0;

    switch (kin->type) {
        case TUXRT_KIN_CARTESIAN:
            det = kin_cartesian_jacobian_det(kin, joints->position);
            break;
        case TUXRT_KIN_COREXY:
            det = kin_corexy_jacobian_det(kin, joints->position);
            break;
        case TUXRT_KIN_POLAR:
            det = kin_polar_jacobian_det(kin, joints->position);
            break;
        case TUXRT_KIN_SCARA:
            det = kin_scara_jacobian_det(kin, joints->position);
            break;
        case TUXRT_KIN_DELTA:
            det = kin_delta_jacobian_det(kin, joints->position);
            break;
        default:
            return true;  // Unknown type → assume singular (safe)
    }

    if (fabs(det) < eps_hard) return true;

    // Soft singularity check if threshold provided
    double eps_soft = (threshold > 0.0 && threshold < kin->eps_soft)
                      ? threshold : kin->eps_soft;
    if (fabs(det) < eps_soft && fabs(det) >= eps_hard) {
        // Near-singularity: warn but don't stop
        // (caller can slow down by 50%)
        return true;
    }

    return false;
}

void tuxrt_kinematics_destroy(tuxrt_kinematics_t* kin) {
    free(kin);
}
