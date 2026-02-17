#include "position_solver.h"
#include "peer_table.h"
#include "nvs_config.h"
#include "bsp.hpp"
#include <Arduino.h>
#include <string.h>
#include <math.h>

static const char* TAG = "solver";

// --- Kalman state per node ---

struct KalmanState {
    float x[3];       // position estimate
    float P[3];       // diagonal covariance (simplified: independent axes)
    bool  initialized;
};

static KalmanState s_kalman[MESH_MAX_NODES];

// --- MDS helpers ---
// Simplified classical MDS for embedded use (no full eigendecomposition library).
// Uses power iteration to extract top eigenvectors from the double-centered
// squared-distance matrix.

// Squared distance matrix (only upper triangle, symmetric)
static float s_D2[MESH_MAX_NODES][MESH_MAX_NODES];

// Double-centered matrix B
static float s_B[MESH_MAX_NODES][MESH_MAX_NODES];

// Eigenvectors (columns) and eigenvalues
static float s_eigvec[MESH_MAX_NODES][3];  // up to 3 dimensions
static float s_eigval[3];

// Temp vectors for power iteration
static float s_tempVec[MESH_MAX_NODES];
static float s_tempVec2[MESH_MAX_NODES];

static void matVecMul(const float B[][MESH_MAX_NODES], const float* v, float* out, int n) {
    for (int i = 0; i < n; i++) {
        out[i] = 0;
        for (int j = 0; j < n; j++) {
            out[i] += B[i][j] * v[j];
        }
    }
}

static float vecNorm(const float* v, int n) {
    float sum = 0;
    for (int i = 0; i < n; i++) sum += v[i] * v[i];
    return sqrtf(sum);
}

static float vecDot(const float* a, const float* b, int n) {
    float sum = 0;
    for (int i = 0; i < n; i++) sum += a[i] * b[i];
    return sum;
}

// Power iteration to find top eigenvector of symmetric matrix B (n×n).
// Returns eigenvalue. Eigenvector stored in out[].
static float powerIteration(float B[][MESH_MAX_NODES], int n, float* out, int maxIter = 100) {
    // Initialize with random-ish vector
    for (int i = 0; i < n; i++) out[i] = 1.0f + 0.1f * i;

    float eigenvalue = 0;
    for (int iter = 0; iter < maxIter; iter++) {
        matVecMul(B, out, s_tempVec, n);
        float norm = vecNorm(s_tempVec, n);
        if (norm < 1e-10f) break;
        for (int i = 0; i < n; i++) out[i] = s_tempVec[i] / norm;
        eigenvalue = norm;
    }
    return eigenvalue;
}

// Deflate: B = B - eigenvalue * v * v^T
static void deflate(float B[][MESH_MAX_NODES], int n, float eigenvalue, const float* v) {
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            B[i][j] -= eigenvalue * v[i] * v[j];
        }
    }
}

// --- Public API ---

void PositionSolver::init() {
    for (int i = 0; i < MESH_MAX_NODES; i++) {
        s_kalman[i].initialized = false;
        s_kalman[i].x[0] = 0;
        s_kalman[i].x[1] = 0;
        s_kalman[i].x[2] = 0;
        s_kalman[i].P[0] = 1000.0f;
        s_kalman[i].P[1] = 1000.0f;
        s_kalman[i].P[2] = 1000.0f;
    }
    Serial.println("[solver] Initialized");
}

void PositionSolver::solve() {
    uint8_t n = PeerTable::peerCount();
    if (n < 2) {
        Serial.println("[solver] Need at least 2 nodes");
        return;
    }

    uint8_t dim = PeerTable::getDimension();
    Serial.printf("[solver] Solving %uD for %u nodes\n", dim, n);

    // Special case: 2 nodes = distance only
    if (n == 2) {
        float dist = PeerTable::getDistance(0, 1);
        if (dist < 0) {
            Serial.println("[solver] No distance between 2 nodes");
            return;
        }
        PeerTable::setPosition(0, 0, 0, 0, 1.0f);
        PeerTable::setPosition(1, dist, 0, 0, 1.0f);
        Serial.printf("[solver] 2-node: A=(0,0,0) B=(%.0f,0,0)\n", dist);
        return;
    }

    // Build squared distance matrix
    int validPairs = 0;
    for (uint8_t i = 0; i < n; i++) {
        s_D2[i][i] = 0;
        for (uint8_t j = i + 1; j < n; j++) {
            float d = PeerTable::getDistance(i, j);
            if (d >= 0) {
                s_D2[i][j] = d * d;
                s_D2[j][i] = d * d;
                validPairs++;
            } else {
                // Missing distance — use large value as placeholder
                s_D2[i][j] = -1;
                s_D2[j][i] = -1;
            }
        }
    }

    int totalPairs = (n * (n - 1)) / 2;
    if (validPairs < (int)(n - 1)) {
        Serial.printf("[solver] Insufficient distances (%d/%d pairs)\n", validPairs, totalPairs);
        return;
    }

    // Fill missing distances with average of known distances (simple imputation)
    float avgD2 = 0;
    int avgCount = 0;
    for (uint8_t i = 0; i < n; i++) {
        for (uint8_t j = i + 1; j < n; j++) {
            if (s_D2[i][j] >= 0) {
                avgD2 += s_D2[i][j];
                avgCount++;
            }
        }
    }
    if (avgCount > 0) avgD2 /= avgCount;

    for (uint8_t i = 0; i < n; i++) {
        for (uint8_t j = i + 1; j < n; j++) {
            if (s_D2[i][j] < 0) {
                s_D2[i][j] = avgD2;
                s_D2[j][i] = avgD2;
            }
        }
    }

    // Double centering: B = -0.5 * J * D² * J, where J = I - (1/n)*11^T
    // Row means, column means, grand mean of D²
    float rowMean[MESH_MAX_NODES];
    float grandMean = 0;
    for (uint8_t i = 0; i < n; i++) {
        rowMean[i] = 0;
        for (uint8_t j = 0; j < n; j++) {
            rowMean[i] += s_D2[i][j];
        }
        rowMean[i] /= n;
        grandMean += rowMean[i];
    }
    grandMean /= n;

    for (uint8_t i = 0; i < n; i++) {
        for (uint8_t j = 0; j < n; j++) {
            s_B[i][j] = -0.5f * (s_D2[i][j] - rowMean[i] - rowMean[j] + grandMean);
        }
    }

    // Extract top eigenvectors via power iteration
    uint8_t numDim = (dim > 3) ? 3 : dim;
    for (uint8_t d = 0; d < numDim; d++) {
        s_eigval[d] = powerIteration(s_B, n, s_eigvec[d] /* reuse column */, 200);

        // Store eigenvector as column d of per-node data
        // (s_eigvec layout: s_eigvec[node][dim] — but powerIteration outputs s_eigvec[d] as a flat array)
        // We need a different layout. Let me use s_tempVec2 as temp and copy.
        // Actually, powerIteration stores into the provided float* which is s_eigvec[d] — an array of MESH_MAX_NODES.
        // But we want s_eigvec[node][dim]. Let me just use separate storage.

        // Actually the layout s_eigvec[dim][node] works fine for power iteration.
        // We'll read it as: coordinate d for node i = s_eigvec[d][i] * sqrt(eigenvalue_d)

        deflate(s_B, n, s_eigval[d], s_eigvec[d] /* temp used the flat array */);
    }

    // Hmm, the above used s_eigvec[d] as float[MESH_MAX_NODES] per dimension. Let me restructure.
    // s_eigvec is declared as [MESH_MAX_NODES][3] but I'm using [d] as the first index
    // where d < 3 and the second index goes up to n < MESH_MAX_NODES.
    // This is a layout mismatch. Let me use separate storage.

    // Actually s_eigvec[MESH_MAX_NODES][3] — I passed &s_eigvec[d][0]... no, that's s_eigvec[d]
    // which is a float[3]. That's wrong — we need float[MESH_MAX_NODES] per dimension.

    // Let me fix: use s_D2 rows as temp storage for eigenvectors since D2 is no longer needed.
    // s_D2[d] is float[MESH_MAX_NODES] — perfect.

    // Redo the eigenvector extraction using s_D2 rows as storage:

    // Re-build B (it was deflated)
    for (uint8_t i = 0; i < n; i++) {
        for (uint8_t j = 0; j < n; j++) {
            // Need original D2 back... we overwrote it. This approach is getting messy.
            // Let me just rebuild from PeerTable distances.
        }
    }

    // --- REDO: cleaner approach using s_D2 rows for eigenvectors ---
    // Rebuild D2
    for (uint8_t i = 0; i < n; i++) {
        s_D2[i][i] = 0;
        for (uint8_t j = i + 1; j < n; j++) {
            float d = PeerTable::getDistance(i, j);
            float d2 = (d >= 0) ? d * d : avgD2;
            s_D2[i][j] = d2;
            s_D2[j][i] = d2;
        }
    }

    // Rebuild B
    for (uint8_t i = 0; i < n; i++) {
        rowMean[i] = 0;
        for (uint8_t j = 0; j < n; j++) rowMean[i] += s_D2[i][j];
        rowMean[i] /= n;
    }
    grandMean = 0;
    for (uint8_t i = 0; i < n; i++) grandMean += rowMean[i];
    grandMean /= n;
    for (uint8_t i = 0; i < n; i++) {
        for (uint8_t j = 0; j < n; j++) {
            s_B[i][j] = -0.5f * (s_D2[i][j] - rowMean[i] - rowMean[j] + grandMean);
        }
    }

    // Use first 3 rows of s_D2 as eigenvector storage (each row = MESH_MAX_NODES floats)
    float* evec[3] = { s_D2[0], s_D2[1], s_D2[2] };
    float evals[3] = {0, 0, 0};

    for (uint8_t d = 0; d < numDim; d++) {
        evals[d] = powerIteration(s_B, n, evec[d], 200);
        deflate(s_B, n, evals[d], evec[d]);
    }

    // Compute coordinates: coord[node][d] = evec[d][node] * sqrt(eigenvalue[d])
    float coords[MESH_MAX_NODES][3];
    memset(coords, 0, sizeof(coords));
    for (uint8_t d = 0; d < numDim; d++) {
        float scale = (evals[d] > 0) ? sqrtf(evals[d]) : 0;
        for (uint8_t i = 0; i < n; i++) {
            coords[i][d] = evec[d][i] * scale;
        }
    }

    // Anchor: gateway (node 0) at origin, first peer along +X
    // Translate so node 0 is at origin
    float offset[3] = { coords[0][0], coords[0][1], coords[0][2] };
    for (uint8_t i = 0; i < n; i++) {
        for (uint8_t d = 0; d < numDim; d++) {
            coords[i][d] -= offset[d];
        }
    }

    // Rotate so node 1 is along +X (if more than 1 node)
    if (n >= 2 && numDim >= 2) {
        float dx = coords[1][0];
        float dy = coords[1][1];
        float r = sqrtf(dx * dx + dy * dy);
        if (r > 1e-6f) {
            float cosA = dx / r;
            float sinA = dy / r;
            // Apply rotation to all nodes
            for (uint8_t i = 0; i < n; i++) {
                float x = coords[i][0];
                float y = coords[i][1];
                coords[i][0] = x * cosA + y * sinA;
                coords[i][1] = -x * sinA + y * cosA;
            }
        }
    }

    // Apply Kalman filter
    float processNoise = (float)NvsConfigManager::ftmKalmanProcessNoise;

    for (uint8_t i = 0; i < n; i++) {
        KalmanState* k = &s_kalman[i];

        if (!k->initialized) {
            // First measurement — initialize directly
            for (int d = 0; d < 3; d++) {
                k->x[d] = coords[i][d];
                k->P[d] = 100.0f;  // initial uncertainty
            }
            k->initialized = true;
        } else {
            // Kalman update (simplified diagonal)
            for (int d = 0; d < (int)numDim; d++) {
                // Predict
                k->P[d] += processNoise;

                // Measurement noise — inversely proportional to number of valid distances
                float R = 50.0f;  // base measurement noise

                // Update
                float K = k->P[d] / (k->P[d] + R);
                float innovation = coords[i][d] - k->x[d];
                k->x[d] += K * innovation;
                k->P[d] *= (1.0f - K);
            }
        }

        // Write filtered position back to PeerTable
        float conf = 1.0f / (1.0f + (k->P[0] + k->P[1] + k->P[2]) / 3.0f);
        PeerTable::setPosition(i, k->x[0], k->x[1], k->x[2], conf);
    }

    Serial.printf("[solver] Positions updated (%uD)\n", numDim);
}

void PositionSolver::reset() {
    for (int i = 0; i < MESH_MAX_NODES; i++) {
        s_kalman[i].initialized = false;
    }
    Serial.println("[solver] Kalman state reset");
}
