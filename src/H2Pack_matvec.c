#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <math.h>
#include <omp.h>

#ifdef USE_MKL
#include <mkl.h>
#endif

#include "H2Pack_config.h"
#include "H2Pack_utils.h"
#include "H2Pack_typedef.h"
#include "H2Pack_aux_structs.h"
#include "x86_intrin_wrapper.h" 

// Calculate GEMV A * x0 and A' * x1 in one run to reduce bandwidth pressure
// Input parameters:
//   nrow   : Number of rows in the matrix
//   ncol   : Number of columns in the matrix
//   mat    : Matrix, size >= nrow * ldm
//   ldm    : Leading dimension of the matrix, >= ncol
//   x_in_0 : Input vector 0
//   x_in_1 : Input vector 1
// Output parameter:
//   x_out_0 : Output vector 0, := mat * x_in_0
//   x_out_1 : Output vector 1, := trans(mat) * x_in_0
static void CBLAS_GESYMMV(
    const int nrow, const int ncol, const DTYPE *mat, const int ldm,
    const DTYPE *x_in_0, const DTYPE *x_in_1, DTYPE *x_out_0, DTYPE *x_out_1
)
{
    const int nrow_2 = (nrow / 2) * 2;
    for (int i = 0; i < nrow_2; i += 2)
    {
        const DTYPE *mat_irow0 = mat + (i + 0) * ldm;
        const DTYPE *mat_irow1 = mat + (i + 1) * ldm;
        const DTYPE x_in_1_i0 = x_in_1[i + 0];
        const DTYPE x_in_1_i1 = x_in_1[i + 1];
        DTYPE sum0 = 0, sum1 = 0;
        #pragma omp simd
        for (int j = 0; j < ncol; j++)
        {
            DTYPE x_in_0_j = x_in_0[j];
            sum0 += mat_irow0[j] * x_in_0_j;
            sum1 += mat_irow1[j] * x_in_0_j;
            DTYPE tmp = x_in_1_i0 * mat_irow0[j];
            tmp += x_in_1_i1 * mat_irow1[j];
            x_out_1[j] += tmp;
        }
        x_out_0[i + 0] += sum0;
        x_out_0[i + 1] += sum1;
    }
    for (int i = nrow_2; i < nrow; i++)
    {
        const DTYPE *mat_irow = mat + i * ldm;
        const DTYPE x_in_1_i = x_in_1[i];
        DTYPE sum = 0;
        #pragma omp simd
        for (int j = 0; j < ncol; j++)
        {
            sum += mat_irow[j] * x_in_0[j];
            x_out_1[j] += x_in_1_i * mat_irow[j];
        }
        x_out_0[i] += sum;
    }
}

// H2 representation matvec upward sweep, calculate U_j^T * x_j
void H2P_matvec_upward_sweep(H2Pack_t h2pack, const DTYPE *x)
{
    int max_child      = h2pack->max_child;
    int n_node         = h2pack->n_node;
    int n_leaf_node    = h2pack->n_leaf_node;
    int max_level      = h2pack->max_level;
    int min_adm_level  = h2pack->min_adm_level;
    int max_adm_height = h2pack->max_adm_height;
    int *children      = h2pack->children;
    int *n_child       = h2pack->n_child;
    int *height_n_node = h2pack->height_n_node;
    int *node_level    = h2pack->node_level;
    int *height_nodes  = h2pack->height_nodes;
    int *mat_cluster   = h2pack->mat_cluster;
    
    // 1. Initialize y0 on the first run
    if (h2pack->y0 == NULL)
    {
        h2pack->y0 = (H2P_dense_mat_t*) malloc(sizeof(H2P_dense_mat_t) * n_node);
        assert(h2pack->y0 != NULL);
        H2P_dense_mat_t *y0 = h2pack->y0;
        H2P_dense_mat_t *U  = h2pack->U;
        for (int node = 0; node < n_node; node++)
        {
            int ncol = U[node]->ncol;
            if (ncol > 0) 
            {
                H2P_dense_mat_init(&y0[node], ncol, 1);
            } else {
                H2P_dense_mat_init(&y0[node], 0, 0);
                y0[node]->nrow = 0;
                y0[node]->ncol = 0;
                y0[node]->ld   = 0;
            }
        }
    }
    
    // 2. Upward sweep
    H2P_dense_mat_t *y0 = h2pack->y0;
    H2P_dense_mat_t *U  = h2pack->U;
    for (int i = 0; i <= max_adm_height; i++)
    {
        int *height_i_nodes = height_nodes + i * n_leaf_node;
        int height_i_n_node = height_n_node[i];
        int nthreads = MIN(height_i_n_node, h2pack->n_thread);
        
        #pragma omp parallel num_threads(nthreads)
        {
            int tid = omp_get_thread_num();
            
            h2pack->tb[tid]->timer = -H2P_get_wtime_sec();
            #pragma omp for schedule(dynamic) nowait
            for (int j = 0; j < height_i_n_node; j++)
            {
                int node  = height_i_nodes[j];
                int level = node_level[node];
                if (level < min_adm_level) continue;
                int n_child_node = n_child[node];
                int *child_nodes = children + node * max_child;
                if (n_child_node == 0)
                {
                    // Leaf node, directly calculate U_j^T * x_j
                    const DTYPE *x_spos = x + mat_cluster[node * 2];
                    CBLAS_GEMV(
                        CblasRowMajor, CblasTrans, U[node]->nrow, U[node]->ncol, 
                        1.0, U[node]->data, U[node]->ld, 
                        x_spos, 1, 0.0, y0[node]->data, 1
                    );
                } else {
                    // Non-leaf node, concatenate children node's y0 and multiple
                    // it with U_j^T
                    int U_srow = 0;
                    for (int k = 0; k < n_child_node; k++)
                    {
                        int child_k = child_nodes[k];
                        int child_k_len = y0[child_k]->nrow; 
                        DTYPE *U_node_k = U[node]->data + U_srow * U[node]->ld;
                        DTYPE beta = (k == 0) ? 0.0 : 1.0;
                        CBLAS_GEMV(
                            CblasRowMajor, CblasTrans, child_k_len, U[node]->ncol, 
                            1.0, U_node_k, U[node]->ld, 
                            y0[child_k]->data, 1, beta, y0[node]->data, 1
                        );
                        U_srow += child_k_len;
                    }
                }  // End of "if (n_child_node == 0)"
            }  // End of j loop
            h2pack->tb[tid]->timer += H2P_get_wtime_sec();
        }  // End of "pragma omp parallel"
        
        #ifdef PROFILING_OUTPUT
        double max_t = 0.0, avg_t = 0.0, min_t = 1145141919.0;
        for (int i = 0; i < nthreads; i++)
        {
            double thread_i_timer = h2pack->tb[i]->timer;
            avg_t += thread_i_timer;
            max_t = MAX(max_t, thread_i_timer);
            min_t = MIN(min_t, thread_i_timer);
        }
        avg_t /= (double) nthreads;
        printf("[PROFILING] MatVec upward sweep: height %d, %d/%d threads, %d nodes, ", i, nthreads, h2pack->n_thread, height_i_n_node);
        printf("min/avg/max thread wall-time = %.3lf, %.3lf, %.3lf (s)\n", min_t, avg_t, max_t);
        #endif
    }  // End of i loop
}

// Initialize auxiliary array y1 used in intermediate sweep
void H2P_matvec_init_y1(H2Pack_t h2pack)
{
    int n_node = h2pack->n_node;
    int n_thread = h2pack->n_thread;
    int *node_n_r_adm = h2pack->node_n_r_adm;
    H2P_dense_mat_t *U = h2pack->U;
    if (h2pack->y1 == NULL)
    {
        h2pack->y1 = (H2P_dense_mat_t*) malloc(sizeof(H2P_dense_mat_t) * n_node);
        assert(h2pack->y1 != NULL);
        for (int i = 0; i < n_node; i++) 
            H2P_dense_mat_init(&h2pack->y1[i], 0, 0);
    }
    H2P_dense_mat_t *y1 = h2pack->y1;
    for (int i = 0; i < n_node; i++) 
    {
        // Use ld to mark if y1[i] is visited in this intermediate sweep
        y1[i]->ld = 0;
        if (node_n_r_adm[i])
        {
            // The first U[node{0, 1}]->ncol elements in y1[node{0, 1}] will be used in downward
            // sweep, store the final results in this part and use the positions behind this as
            // thread buffers. The last position of each row is used to mark if this row has data.
            H2P_dense_mat_resize(y1[i], n_thread, U[i]->ncol + 1);
            y1[i]->ld = 1;
        }
    }
}

// Sum thread-local buffers to obtain final y1 results
void H2P_matvec_sum_y1_thread(H2Pack_t h2pack)
{
    int n_node = h2pack->n_node;
    int n_thread = h2pack->n_thread;
    H2P_dense_mat_t *y1 = h2pack->y1;
    #pragma omp parallel num_threads(n_thread)
    {
        int tid = omp_get_thread_num();
        
        h2pack->tb[tid]->timer -= H2P_get_wtime_sec();
        #pragma omp for schedule(dynamic) nowait
        for (int i = 0; i < n_node; i++)
        {
            if (y1[i]->ld == 0) continue;
            int ncol = y1[i]->ncol;
            DTYPE *dst_row = y1[i]->data;
            for (int j = 1; j < n_thread; j++)
            {
                DTYPE *src_row = y1[i]->data + j * ncol;
                if (src_row[ncol - 1] != 1.0) continue;
                #pragma omp simd
                for (int k = 0; k < ncol - 1; k++)
                    dst_row[k] += src_row[k];
            }
        }
        h2pack->tb[tid]->timer += H2P_get_wtime_sec();
    }
}

// H2 representation matvec intermediate sweep, calculate B_{ij} * (U_j^T * x_j)
// All B_{ij} matrices have been calculated and stored
void H2P_matvec_intermediate_sweep_AOT(H2Pack_t h2pack, const DTYPE *x)
{
    int    n_node        = h2pack->n_node;
    int    n_thread      = h2pack->n_thread;
    int    n_r_adm_pair  = h2pack->n_r_adm_pair;
    int    *r_adm_pairs  = h2pack->r_adm_pairs;
    int    *node_level   = h2pack->node_level;
    int    *mat_cluster  = h2pack->mat_cluster;
    int    *node_n_r_adm = h2pack->node_n_r_adm;
    int    *B_nrow       = h2pack->B_nrow;
    int    *B_ncol       = h2pack->B_ncol;
    size_t *B_ptr        = h2pack->B_ptr;
    DTYPE  *B_data       = h2pack->B_data;
    H2P_int_vec_t B_blk  = h2pack->B_blk;
    H2P_dense_mat_t *y0  = h2pack->y0;
    H2P_dense_mat_t *U   = h2pack->U;

    // 1. Initialize y1 
    H2P_matvec_init_y1(h2pack);
    H2P_dense_mat_t *y1 = h2pack->y1;

    // 2. Intermediate sweep
    const int n_B_blk = B_blk->length;
    #pragma omp parallel num_threads(n_thread)
    {
        int tid = omp_get_thread_num();
        DTYPE *y = h2pack->tb[tid]->y;
        
        h2pack->tb[tid]->timer = -H2P_get_wtime_sec();
        #pragma omp for schedule(static)
        for (int i = 0; i < n_node; i++)
        {
            if (y1[i]->ld == 0) continue;
            const int ncol = y1[i]->ncol;
            // Need not to reset all copies of y1 to be 0 here, use the last element in
            // each row as the beta value to rewrite / accumulate y1 results in GEMV
            memset(y1[i]->data, 0, sizeof(DTYPE) * ncol);
            for (int j = 1; j < n_thread; j++)
                y1[i]->data[(j + 1) * ncol - 1] = 0.0;
        }
        
        #pragma omp barrier
        
        //#pragma omp for schedule(dynamic) nowait
        //for (int i_blk = 0; i_blk < n_B_blk; i_blk++)
        int i_blk = tid;    // Use first-touch policy for better NUMA memeory access performance
        {
            int B_blk_s = B_blk->data[i_blk];
            int B_blk_e = B_blk->data[i_blk + 1];
            for (int i = B_blk_s; i < B_blk_e; i++)
            {
                int node0  = r_adm_pairs[2 * i];
                int node1  = r_adm_pairs[2 * i + 1];
                int level0 = node_level[node0];
                int level1 = node_level[node1];
                
                DTYPE *Bi = B_data + B_ptr[i];
                int Bi_nrow = B_nrow[i];
                int Bi_ncol = B_ncol[i];
                
                // (1) Two nodes are of the same level, compress on both sides
                if (level0 == level1)
                {
                    int ncol0 = y1[node0]->ncol;
                    int ncol1 = y1[node1]->ncol;
                    DTYPE *y1_dst_0 = y1[node0]->data + tid * ncol0;
                    DTYPE *y1_dst_1 = y1[node1]->data + tid * ncol1;
                    DTYPE beta0 = y1_dst_0[ncol0 - 1];
                    DTYPE beta1 = y1_dst_1[ncol1 - 1];
                    y1_dst_0[ncol0 - 1] = 1.0;
                    y1_dst_1[ncol1 - 1] = 1.0;
                    
                    if (beta0 == 0.0) memset(y1_dst_0, 0, sizeof(DTYPE) * Bi_nrow);
                    if (beta1 == 0.0) memset(y1_dst_1, 0, sizeof(DTYPE) * Bi_ncol);
                    CBLAS_GESYMMV(
                        Bi_nrow, Bi_ncol, Bi, Bi_ncol,
                        y0[node1]->data, y0[node0]->data, y1_dst_0, y1_dst_1
                    );
                }
                
                // (2) node1 is a leaf node and its level is higher than node0's level, 
                //     only compressed on node0's side, node1's side don't need the 
                //     downward sweep and can directly accumulate result to output vector
                if (level0 > level1)
                {
                    int vec_s1 = mat_cluster[node1 * 2];
                    DTYPE       *y_spos = y + vec_s1;
                    const DTYPE *x_spos = x + vec_s1;
                    
                    int ncol0       = y1[node0]->ncol;
                    DTYPE *y1_dst_0 = y1[node0]->data + tid * ncol0;
                    DTYPE beta0     = y1_dst_0[ncol0 - 1];
                    y1_dst_0[ncol0 - 1] = 1.0;
                    
                    if (beta0 == 0.0) memset(y1_dst_0, 0, sizeof(DTYPE) * Bi_nrow);
                    CBLAS_GESYMMV(
                        Bi_nrow, Bi_ncol, Bi, Bi_ncol,
                        x_spos, y0[node0]->data, y1_dst_0, y_spos
                    );
                }
                
                // (3) node0 is a leaf node and its level is higher than node1's level, 
                //     only compressed on node1's side, node0's side don't need the 
                //     downward sweep and can directly accumulate result to output vector
                if (level0 < level1)
                {
                    int vec_s0 = mat_cluster[node0 * 2];
                    DTYPE       *y_spos = y + vec_s0;
                    const DTYPE *x_spos = x + vec_s0;
                    
                    int ncol1       = y1[node1]->ncol;
                    DTYPE *y1_dst_1 = y1[node1]->data + tid * ncol1;
                    DTYPE beta1     = y1_dst_1[ncol1 - 1];
                    y1_dst_1[ncol1 - 1] = 1.0;
                    
                    if (beta1 == 0.0) memset(y1_dst_1, 0, sizeof(DTYPE) * Bi_ncol);
                    CBLAS_GESYMMV(
                        Bi_nrow, Bi_ncol, Bi, Bi_ncol,
                        y0[node1]->data, x_spos, y_spos, y1_dst_1
                    );
                }
            }  // End of i loop
        }  // End of i_blk loop
        h2pack->tb[tid]->timer += H2P_get_wtime_sec();
    }  // End of "pragma omp parallel"
    
    // 3. Sum thread-local buffers in y1
    H2P_matvec_sum_y1_thread(h2pack);
    
    #ifdef PROFILING_OUTPUT
    double max_t = 0.0, avg_t = 0.0, min_t = 1145141919.0;
    for (int i = 0; i < n_thread; i++)
    {
        double thread_i_timer = h2pack->tb[i]->timer;
        avg_t += thread_i_timer;
        max_t = MAX(max_t, thread_i_timer);
        min_t = MIN(min_t, thread_i_timer);
    }
    avg_t /= (double) n_thread;
    printf("[PROFILING] MatVec intermediate sweep: min/avg/max thread wall-time = %.3lf, %.3lf, %.3lf (s)\n", min_t, avg_t, max_t);
    #endif
}

// Extend the number of points to a multipler of SIMD_LEN and perform a symmetric n-body matvec
// Input parameters:
//   coord0      : Matrix, size dim-by-ld0, coordinates of the 1st point set
//   ld0         : Leading dimension of coord0, should be >= n0
//   n0          : Number of points in coord0 (each column in coord0 is a coordinate)
//   coord1      : Matrix, size dim-by-ld1, coordinates of the 2nd point set
//   ld1         : Leading dimension of coord1, should be >= n1
//   n1          : Number of points in coord1 (each column in coord0 is a coordinate)
//   x_in_0_     : Vector, size >= n1, will be left multiplied by kernel_matrix(coord0, coord1)
//   x_in_1_     : Vector, size >= n0, will be left multiplied by kernel_matrix(coord0, coord1)^T.
//                 If x_in_1_ == NULL, x_out_1 will remains unchanged.
//   pt_dim      : Dimension of point coordinate
//   krnl_dim    : Dimension of tensor kernel's return
//   workbuf     : H2P_dense_mat data sturcture for allocating working buffer
//   krnl_matvec : Pointer to kernel matrix matvec function
// Output parameter:
//   x_out_0_ : Vector, size >= n0, x_out_0 += kernel_matrix(coord0, coord1) * x_in_0
//   x_out_1_ : Vector, size >= n1, x_out_1 += kernel_matrix(coord0, coord1)^T * x_in_1
void H2P_ext_symm_krnl_matvec(
    DTYPE *coord0, const int ld0, const int n0,
    DTYPE *coord1, const int ld1, const int n1,
    const DTYPE *x_in_0_, const DTYPE *x_in_1_, DTYPE *x_out_0_, DTYPE *x_out_1_,
    const int pt_dim, const int krnl_dim, H2P_dense_mat_t workbuf, 
    kernel_matvec_fptr krnl_matvec
)
{
    int n0_ext  = (n0 + SIMD_LEN - 1) / SIMD_LEN * SIMD_LEN;
    int n1_ext  = (n1 + SIMD_LEN - 1) / SIMD_LEN * SIMD_LEN;
    int n01_ext = n0_ext + n1_ext;
    int buf_size = (pt_dim + krnl_dim) * n01_ext * 2;
    H2P_dense_mat_resize(workbuf, 1, buf_size);
    DTYPE *trg_coord = workbuf->data;
    DTYPE *src_coord = trg_coord + pt_dim * n0_ext;
    DTYPE *x_in_0    = src_coord + pt_dim * n1_ext;
    DTYPE *x_in_1    = x_in_0    + n1_ext * krnl_dim;
    DTYPE *x_out_0   = x_in_1    + n0_ext * krnl_dim;
    DTYPE *x_out_1   = x_out_0   + n0_ext * krnl_dim;
    
    // Copy coorindates and pad the extend part
    for (int i = 0; i < pt_dim; i++)
    {
        DTYPE *c0_src = coord0 + i * ld0;
        DTYPE *c1_src = coord1 + i * ld1;
        DTYPE *c0_dst = trg_coord + i * n0_ext;
        DTYPE *c1_dst = src_coord + i * n1_ext;
        memcpy(c0_dst, c0_src, sizeof(DTYPE) * n0);
        memcpy(c1_dst, c1_src, sizeof(DTYPE) * n1);
        // Use an extremely large coordinate so the inverse distance of these 
        // extra points to original points are numerically zero
        for (int j = n0; j < n0_ext; j++) c0_dst[j] = 1e100;
        for (int j = n1; j < n1_ext; j++) c1_dst[j] = 1e100;
    }
    
    // Copy input vectors and initialize output vectors
    memcpy(x_in_0, x_in_0_, sizeof(DTYPE) * n1 * krnl_dim);
    memset(x_out_0, 0, sizeof(DTYPE) * n0_ext * krnl_dim);
    for (int j = n1 * krnl_dim; j < n1_ext * krnl_dim; j++) x_in_0[j] = 0;
    if (x_in_1_ != NULL)
    {
        memcpy(x_in_1, x_in_1_, sizeof(DTYPE) * n0 * krnl_dim);
        memset(x_out_1, 0, sizeof(DTYPE) * n1_ext * krnl_dim);
        for (int j = n0 * krnl_dim; j < n0_ext * krnl_dim; j++) x_in_1[j] = 0;
    } else {
        x_in_1  = NULL;
        x_out_1 = NULL;
    }
    
    // Do the symmetric n-body matvec
    krnl_matvec(
        trg_coord, n0_ext, n0_ext,
        src_coord, n1_ext, n1_ext,
        x_in_0, x_in_1, x_out_0, x_out_1
    );
    
    // Add results back to original output vectors
    #pragma omp simd
    for (int i = 0; i < n0 * krnl_dim; i++) x_out_0_[i] += x_out_0[i];
    if (x_in_1_ != NULL)
    {
        #pragma omp simd
        for (int i = 0; i < n1 * krnl_dim; i++) x_out_1_[i] += x_out_1[i];
    }
}

// H2 representation matvec intermediate sweep, calculate B_{ij} * (U_j^T * x_j)
// Need to calculate all B_{ij} matrices before using it
void H2P_matvec_intermediate_sweep_JIT(H2Pack_t h2pack, const DTYPE *x)
{
    int    pt_dim        = h2pack->pt_dim;
    int    krnl_dim      = h2pack->krnl_dim;
    int    n_node        = h2pack->n_node;
    int    n_point       = h2pack->n_point;
    int    n_thread      = h2pack->n_thread;
    int    n_r_adm_pair  = h2pack->n_r_adm_pair;
    int    *r_adm_pairs  = h2pack->r_adm_pairs;
    int    *node_level   = h2pack->node_level;
    int    *pt_cluster   = h2pack->pt_cluster;
    int    *mat_cluster  = h2pack->mat_cluster;
    int    *node_n_r_adm = h2pack->node_n_r_adm;
    int    *B_nrow       = h2pack->B_nrow;
    int    *B_ncol       = h2pack->B_ncol;
    DTYPE  *coord        = h2pack->coord;
    H2P_int_vec_t B_blk  = h2pack->B_blk;
    H2P_dense_mat_t *y0  = h2pack->y0;
    H2P_dense_mat_t *U   = h2pack->U;
    H2P_dense_mat_t *J_coord = h2pack->J_coord;
    kernel_eval_fptr   krnl_eval   = h2pack->krnl_eval;
    kernel_matvec_fptr krnl_matvec = h2pack->krnl_matvec;

    // 1. Initialize y1 
    H2P_matvec_init_y1(h2pack);
    H2P_dense_mat_t *y1 = h2pack->y1;

    // 2. Intermediate sweep
    const int n_B_blk = B_blk->length;
    #pragma omp parallel num_threads(n_thread)
    {
        int tid = omp_get_thread_num();
        H2P_dense_mat_t Bi = h2pack->tb[tid]->mat0;
        DTYPE *y = h2pack->tb[tid]->y;
        
        H2P_dense_mat_t workbuf = h2pack->tb[tid]->mat1;
        
        h2pack->tb[tid]->timer = -H2P_get_wtime_sec();
        #pragma omp for schedule(static)
        for (int i = 0; i < n_node; i++)
        {
            if (y1[i]->ld == 0) continue;
            const int ncol = y1[i]->ncol;
            // Need not to reset all copies of y1 to be 0 here, use the last element in
            // each row as the beta value to rewrite / accumulate y1 results in GEMV
            memset(y1[i]->data, 0, sizeof(DTYPE) * ncol);
            for (int j = 1; j < n_thread; j++)
                y1[i]->data[(j + 1) * ncol - 1] = 0.0;
        }
        
        #pragma omp barrier
        
        #pragma omp for schedule(dynamic) nowait
        for (int i_blk = 0; i_blk < n_B_blk; i_blk++)
        {
            int B_blk_s = B_blk->data[i_blk];
            int B_blk_e = B_blk->data[i_blk + 1];
            for (int i = B_blk_s; i < B_blk_e; i++)
            {
                int node0   = r_adm_pairs[2 * i];
                int node1   = r_adm_pairs[2 * i + 1];
                int level0  = node_level[node0];
                int level1  = node_level[node1];
                int Bi_nrow = B_nrow[i];
                int Bi_ncol = B_ncol[i];

                int Bi_nrow_128KB = (128 * 1024) / (sizeof(DTYPE) * Bi_ncol);
                int Bi_blk_npt = Bi_nrow_128KB / krnl_dim;
                Bi_nrow_128KB = Bi_blk_npt * krnl_dim;
                H2P_dense_mat_resize(Bi, Bi_nrow_128KB, Bi_ncol);
                
                // (1) Two nodes are of the same level, compress on both sides
                if (level0 == level1)
                {
                    int ncol0 = y1[node0]->ncol;
                    int ncol1 = y1[node1]->ncol;
                    DTYPE *y1_dst_0 = y1[node0]->data + tid * ncol0;
                    DTYPE *y1_dst_1 = y1[node1]->data + tid * ncol1;
                    DTYPE beta0 = y1_dst_0[ncol0 - 1];
                    DTYPE beta1 = y1_dst_1[ncol1 - 1];
                    y1_dst_0[ncol0 - 1] = 1.0;
                    y1_dst_1[ncol1 - 1] = 1.0;

                    if (beta0 == 0.0) memset(y1_dst_0, 0, sizeof(DTYPE) * Bi_nrow);
                    if (beta1 == 0.0) memset(y1_dst_1, 0, sizeof(DTYPE) * Bi_ncol);
                    if (krnl_matvec != NULL)
                    {
                        H2P_ext_symm_krnl_matvec(
                            J_coord[node0]->data, J_coord[node0]->ncol, J_coord[node0]->ncol,
                            J_coord[node1]->data, J_coord[node1]->ncol, J_coord[node1]->ncol,
                            y0[node1]->data, y0[node0]->data, y1_dst_0, y1_dst_1,
                            pt_dim, krnl_dim, workbuf, krnl_matvec
                        );
                    } else {
                        int Bi_npt_row = J_coord[node0]->ncol;
                        for (int blk_pt_s = 0; blk_pt_s < Bi_npt_row; blk_pt_s += Bi_blk_npt)
                        {
                            int blk_npt = (blk_pt_s + Bi_blk_npt) > Bi_npt_row ? Bi_npt_row - blk_pt_s : Bi_blk_npt;
                            int blk_srow = blk_pt_s * krnl_dim;
                            int blk_nrow = blk_npt * krnl_dim;
                            krnl_eval(
                                J_coord[node0]->data + blk_pt_s, J_coord[node0]->ncol, blk_npt,
                                J_coord[node1]->data, J_coord[node1]->ncol, J_coord[node1]->ncol,
                                Bi->data, Bi_ncol
                            );
                            CBLAS_GESYMMV(
                                blk_nrow, Bi_ncol, Bi->data, Bi_ncol,
                                y0[node1]->data, y0[node0]->data + blk_srow, y1_dst_0 + blk_srow, y1_dst_1
                            );
                        }
                    }
                }
                
                // (2) node1 is a leaf node and its level is higher than node0's level, 
                //     only compressed on node0's side, node1's side don't need the 
                //     downward sweep and can directly accumulate result to output vector
                if (level0 > level1)
                {
                    int pt_s1 = pt_cluster[node1 * 2];
                    int node1_npt = pt_cluster[node1 * 2 + 1] - pt_s1 + 1;
                    
                    int vec_s1 = mat_cluster[node1 * 2];
                    const DTYPE *x_spos = x + vec_s1;
                    DTYPE       *y_spos = y + vec_s1;
                    
                    int ncol0 = y1[node0]->ncol;
                    DTYPE *y1_dst_0     = y1[node0]->data + tid * ncol0;
                    DTYPE beta0         = y1_dst_0[ncol0 - 1];
                    y1_dst_0[ncol0 - 1] = 1.0;
                    
                    if (beta0 == 0.0) memset(y1_dst_0, 0, sizeof(DTYPE) * Bi_nrow);
                    if (krnl_matvec != NULL)
                    {
                        H2P_ext_symm_krnl_matvec(
                            J_coord[node0]->data, J_coord[node0]->ncol, J_coord[node0]->ncol,
                            coord + pt_s1, n_point, node1_npt,
                            x_spos, y0[node0]->data, y1_dst_0, y_spos, 
                            pt_dim, krnl_dim, workbuf, krnl_matvec
                        );
                    } else {
                        int Bi_npt_row = J_coord[node0]->ncol;
                        for (int blk_pt_s = 0; blk_pt_s < Bi_npt_row; blk_pt_s += Bi_blk_npt)
                        {
                            int blk_npt = (blk_pt_s + Bi_blk_npt) > Bi_npt_row ? Bi_npt_row - blk_pt_s : Bi_blk_npt;
                            int blk_srow = blk_pt_s * krnl_dim;
                            int blk_nrow = blk_npt * krnl_dim;
                            krnl_eval(
                                J_coord[node0]->data + blk_pt_s, J_coord[node0]->ncol, blk_npt,
                                coord + pt_s1, n_point, node1_npt, 
                                Bi->data, Bi_ncol
                            );
                            CBLAS_GESYMMV(
                                blk_nrow, Bi_ncol, Bi->data, Bi_ncol,
                                x_spos, y0[node0]->data + blk_srow, y1_dst_0 + blk_srow, y_spos
                            );
                        }
                    }
                }
                
                // (3) node0 is a leaf node and its level is higher than node1's level, 
                //     only compressed on node1's side, node0's side don't need the 
                //     downward sweep and can directly accumulate result to output vector
                if (level0 < level1)
                {
                    int pt_s0 = pt_cluster[node0 * 2];
                    int node0_npt = pt_cluster[node0 * 2 + 1] - pt_s0 + 1;
                    
                    int vec_s0 = mat_cluster[node0 * 2];
                    const DTYPE *x_spos = x + vec_s0;
                    DTYPE       *y_spos = y + vec_s0;
                    
                    int ncol1 = y1[node1]->ncol;
                    DTYPE *y1_dst_1     = y1[node1]->data + tid * ncol1;
                    DTYPE beta1         = y1_dst_1[ncol1 - 1];
                    y1_dst_1[ncol1 - 1] = 1.0;
                    
                    if (beta1 == 0.0) memset(y1_dst_1, 0, sizeof(DTYPE) * Bi_ncol);
                    if (krnl_matvec != NULL)
                    {
                        H2P_ext_symm_krnl_matvec(
                            coord + pt_s0, n_point, node0_npt,
                            J_coord[node1]->data, J_coord[node1]->ncol, J_coord[node1]->ncol,
                            y0[node1]->data, x_spos, y_spos, y1_dst_1,
                            pt_dim, krnl_dim, workbuf, krnl_matvec
                        );
                    } else {
                        int Bi_npt_row = node0_npt;
                        for (int blk_pt_s = 0; blk_pt_s < Bi_npt_row; blk_pt_s += Bi_blk_npt)
                        {
                            int blk_npt = (blk_pt_s + Bi_blk_npt) > Bi_npt_row ? Bi_npt_row - blk_pt_s : Bi_blk_npt;
                            int blk_srow = blk_pt_s * krnl_dim;
                            int blk_nrow = blk_npt * krnl_dim;
                            krnl_eval(
                                coord + pt_s0 + blk_pt_s, n_point, blk_npt, 
                                J_coord[node1]->data, J_coord[node1]->ncol, J_coord[node1]->ncol,
                                Bi->data, Bi_ncol
                            );
                            CBLAS_GESYMMV(
                                blk_nrow, Bi_ncol, Bi->data, Bi_ncol,
                                y0[node1]->data, x_spos + blk_srow, y_spos + blk_srow, y1_dst_1
                            );
                        }
                    }
                }
            }  // End of i loop
        }  // End of i_blk loop
        h2pack->tb[tid]->timer += H2P_get_wtime_sec();
    }  // End of "pragma omp parallel"
    
    // 3. Sum thread-local buffers in y1
    H2P_matvec_sum_y1_thread(h2pack);
    
    #ifdef PROFILING_OUTPUT
    double max_t = 0.0, avg_t = 0.0, min_t = 1145141919.0;
    for (int i = 0; i < n_thread; i++)
    {
        double thread_i_timer = h2pack->tb[i]->timer;
        avg_t += thread_i_timer;
        max_t = MAX(max_t, thread_i_timer);
        min_t = MIN(min_t, thread_i_timer);
    }
    avg_t /= (double) n_thread;
    printf("[PROFILING] MatVec intermediate sweep: min/avg/max thread wall-time = %.3lf, %.3lf, %.3lf (s)\n", min_t, avg_t, max_t);
    #endif
}

// H2 representation matvec downward sweep, calculate U_i * (B_{ij} * (U_j^T * x_j))
void H2P_matvec_downward_sweep(H2Pack_t h2pack, const DTYPE *x)
{
    int max_child       = h2pack->max_child;
    int n_leaf_node     = h2pack->n_leaf_node;
    int max_level       = h2pack->max_level;
    int min_adm_level   = h2pack->min_adm_level;
    int *children       = h2pack->children;
    int *n_child        = h2pack->n_child;
    int *level_n_node   = h2pack->level_n_node;
    int *level_nodes    = h2pack->level_nodes;
    int *mat_cluster    = h2pack->mat_cluster;
    H2P_dense_mat_t *U  = h2pack->U;
    H2P_dense_mat_t *y1 = h2pack->y1;
    
    for (int i = min_adm_level; i <= max_level; i++)
    {
        int *level_i_nodes = level_nodes + i * n_leaf_node;
        int level_i_n_node = level_n_node[i];
        int nthreads = MIN(level_i_n_node, h2pack->n_thread);
        
        #pragma omp parallel num_threads(nthreads) 
        {
            int tid = omp_get_thread_num();
            DTYPE *y = h2pack->tb[tid]->y;
            H2P_dense_mat_t y1_tmp = h2pack->tb[tid]->mat0;
            
            h2pack->tb[tid]->timer = -H2P_get_wtime_sec();
            #pragma omp for schedule(dynamic) nowait
            for (int j = 0; j < level_i_n_node; j++)
            {
                int node = level_i_nodes[j];
                int n_child_node = n_child[node];
                int *child_nodes = children + node * max_child;
                
                if (y1[node]->ld == 0) continue;
                
                CBLAS_GEMV(
                    CblasRowMajor, CblasNoTrans, U[node]->nrow, U[node]->ncol,
                    1.0, U[node]->data, U[node]->ld, 
                    y1[node]->data, 1, 0.0, y1_tmp->data, 1
                );
                
                if (n_child_node == 0)
                {
                    // Leaf node, accumulate final results to output vector
                    int s_index = mat_cluster[2 * node];
                    int e_index = mat_cluster[2 * node + 1];
                    int n_point = e_index - s_index + 1;
                    DTYPE *y_spos = y + s_index;
                    #pragma omp simd
                    for (int k = 0; k < n_point; k++)
                        y_spos[k] += y1_tmp->data[k];
                } else {
                    // Non-leaf node, push down y1 values
                    int y1_tmp_idx = 0;
                    for (int k = 0; k < n_child_node; k++)
                    {
                        int child_k = child_nodes[k];
                        int child_k_len = U[child_k]->ncol;
                        DTYPE *y1_tmp_spos = y1_tmp->data + y1_tmp_idx;
                        if (y1[child_k]->ld == 0)
                        {
                            H2P_dense_mat_resize(y1[child_k], child_k_len, 1);
                            memcpy(y1[child_k]->data, y1_tmp_spos, sizeof(DTYPE) * child_k_len);
                        } else {
                            #pragma omp simd
                            for (int l = 0; l < child_k_len; l++)
                                y1[child_k]->data[l] += y1_tmp_spos[l];
                        }
                        y1_tmp_idx += child_k_len;
                    }
                }  // End of "if (n_child_node == 0)"
            }  // End of j loop
            h2pack->tb[tid]->timer += H2P_get_wtime_sec();
        }  // End of "pragma omp parallel"
        #ifdef PROFILING_OUTPUT
        double max_t = 0.0, avg_t = 0.0, min_t = 1145141919.0;
        for (int i = 0; i < nthreads; i++)
        {
            double thread_i_timer = h2pack->tb[i]->timer;
            avg_t += thread_i_timer;
            max_t = MAX(max_t, thread_i_timer);
            min_t = MIN(min_t, thread_i_timer);
        }
        avg_t /= (double) nthreads;
        printf("[PROFILING] MatVec downward sweep: level %d, %d/%d threads, %d nodes, ", i, nthreads, h2pack->n_thread, level_i_n_node);
        printf("min/avg/max thread wall-time = %.3lf, %.3lf, %.3lf (s)\n", min_t, avg_t, max_t);
        #endif
    }  // End of i loop
}

// H2 representation matvec dense blocks matvec, calculate D_{ij} * x_j
// All D_{ij} matrices have been calculated and stored
void H2P_matvec_dense_blocks_AOT(H2Pack_t h2pack, const DTYPE *x)
{
    int    n_leaf_node    = h2pack->n_leaf_node;
    int    n_r_inadm_pair = h2pack->n_r_inadm_pair;
    int    *r_inadm_pairs = h2pack->r_inadm_pairs;
    int    *leaf_nodes    = h2pack->height_nodes;
    int    *mat_cluster   = h2pack->mat_cluster;
    int    *D_nrow        = h2pack->D_nrow;
    int    *D_ncol        = h2pack->D_ncol;
    size_t *D_ptr         = h2pack->D_ptr;
    DTYPE  *D_data        = h2pack->D_data;
    H2P_int_vec_t D_blk0  = h2pack->D_blk0;
    H2P_int_vec_t D_blk1  = h2pack->D_blk1;
    
    const int n_D0_blk = D_blk0->length;
    const int n_D1_blk = D_blk1->length;
    #pragma omp parallel num_threads(h2pack->n_thread)
    {
        int tid = omp_get_thread_num();
        DTYPE *y = h2pack->tb[tid]->y;
        
        h2pack->tb[tid]->timer = -H2P_get_wtime_sec();
        // 1. Diagonal blocks matvec
        //#pragma omp for schedule(dynamic) nowait
        //for (int i_blk0 = 0; i_blk0 < n_D0_blk; i_blk0++)
        int i_blk0 = tid;    // Use first-touch policy for better NUMA memeory access performance
        {
            int D_blk0_s = D_blk0->data[i_blk0];
            int D_blk0_e = D_blk0->data[i_blk0 + 1];
            for (int i = D_blk0_s; i < D_blk0_e; i++)
            {
                int node  = leaf_nodes[i];
                int vec_s = mat_cluster[node * 2];
                DTYPE       *y_spos = y + vec_s;
                const DTYPE *x_spos = x + vec_s;
                
                DTYPE *Di = D_data + D_ptr[i];
                int Di_nrow = D_nrow[i];
                int Di_ncol = D_ncol[i];
                
                CBLAS_GEMV(
                    CblasRowMajor, CblasNoTrans, Di_nrow, Di_ncol,
                    1.0, Di, Di_ncol, x_spos, 1, 1.0, y_spos, 1
                );
            }
        }  // End of i_blk0 loop 
        
        // 2. Off-diagonal blocks from inadmissible pairs matvec
        //#pragma omp for schedule(dynamic) nowait
        //for (int i_blk1 = 0; i_blk1 < n_D1_blk; i_blk1++)
        int i_blk1 = tid;    // Use first-touch policy for better NUMA memeory access performance
        {
            int D_blk1_s = D_blk1->data[i_blk1];
            int D_blk1_e = D_blk1->data[i_blk1 + 1];
            for (int i = D_blk1_s; i < D_blk1_e; i++)
            {
                int node0  = r_inadm_pairs[2 * i];
                int node1  = r_inadm_pairs[2 * i + 1];
                int vec_s0 = mat_cluster[2 * node0];
                int vec_s1 = mat_cluster[2 * node1];
                DTYPE       *y_spos0 = y + vec_s0;
                DTYPE       *y_spos1 = y + vec_s1;
                const DTYPE *x_spos0 = x + vec_s0;
                const DTYPE *x_spos1 = x + vec_s1;
                
                DTYPE *Di = D_data + D_ptr[n_leaf_node + i];
                int Di_nrow = D_nrow[n_leaf_node + i];
                int Di_ncol = D_ncol[n_leaf_node + i];
                
                CBLAS_GESYMMV(
                    Di_nrow, Di_ncol, Di, Di_ncol,
                    x_spos1, x_spos0, y_spos0, y_spos1
                );
            }
        }  // End of i_blk1 loop 
        h2pack->tb[tid]->timer += H2P_get_wtime_sec();
    }  // End of "pragma omp parallel"
    
    #ifdef PROFILING_OUTPUT
    double max_t = 0.0, avg_t = 0.0, min_t = 1145141919.0;
    for (int i = 0; i < h2pack->n_thread; i++)
    {
        double thread_i_timer = h2pack->tb[i]->timer;
        avg_t += thread_i_timer;
        max_t = MAX(max_t, thread_i_timer);
        min_t = MIN(min_t, thread_i_timer);
    }
    avg_t /= (double) h2pack->n_thread;
    printf("[PROFILING] MatVec dense block sweep: min/avg/max thread wall-time = %.3lf, %.3lf, %.3lf (s)\n", min_t, avg_t, max_t);
    #endif
}

// H2 representation matvec dense blocks matvec, calculate D_{ij} * x_j
// Need to calculate all D_{ij} matrices before using it
void H2P_matvec_dense_blocks_JIT(H2Pack_t h2pack, const DTYPE *x)
{
    int    pt_dim          = h2pack->pt_dim;
    int    krnl_dim        = h2pack->krnl_dim;
    int    n_point         = h2pack->n_point;
    int    n_leaf_node     = h2pack->n_leaf_node;
    int    n_r_inadm_pair  = h2pack->n_r_inadm_pair;
    int    *r_inadm_pairs  = h2pack->r_inadm_pairs;
    int    *leaf_nodes     = h2pack->height_nodes;
    int    *pt_cluster     = h2pack->pt_cluster;
    int    *mat_cluster    = h2pack->mat_cluster;
    int    *D_nrow         = h2pack->D_nrow;
    int    *D_ncol         = h2pack->D_ncol;
    DTYPE  *coord          = h2pack->coord;
    H2P_int_vec_t    D_blk0 = h2pack->D_blk0;
    H2P_int_vec_t    D_blk1 = h2pack->D_blk1;
    kernel_eval_fptr   krnl_eval   = h2pack->krnl_eval;
    kernel_matvec_fptr krnl_matvec = h2pack->krnl_matvec;
    
    const int n_D0_blk = D_blk0->length;
    const int n_D1_blk = D_blk1->length;
    #pragma omp parallel num_threads(h2pack->n_thread)
    {
        int tid = omp_get_thread_num();
        H2P_dense_mat_t Di = h2pack->tb[tid]->mat0;
        DTYPE *y = h2pack->tb[tid]->y;
        
        H2P_dense_mat_t workbuf = h2pack->tb[tid]->mat1;
        
        h2pack->tb[tid]->timer = -H2P_get_wtime_sec();
        // 1. Diagonal blocks matvec
        #pragma omp for schedule(dynamic) nowait
        for (int i_blk0 = 0; i_blk0 < n_D0_blk; i_blk0++)
        {
            int D_blk0_s = D_blk0->data[i_blk0];
            int D_blk0_e = D_blk0->data[i_blk0 + 1];
            for (int i = D_blk0_s; i < D_blk0_e; i++)
            {
                int node  = leaf_nodes[i];
                int pt_s  = pt_cluster[node * 2];
                int vec_s = mat_cluster[node * 2];
                int node_npt = pt_cluster[node * 2 + 1] - pt_s + 1;
                DTYPE       *y_spos = y + vec_s;
                const DTYPE *x_spos = x + vec_s;
                
                if (krnl_matvec != NULL)
                {
                    H2P_ext_symm_krnl_matvec(
                        coord + pt_s, n_point, node_npt,
                        coord + pt_s, n_point, node_npt,
                        x_spos, NULL, y_spos, NULL, 
                        pt_dim, krnl_dim, workbuf, krnl_matvec
                    );
                } else {
                    int Di_nrow = D_nrow[i];
                    int Di_ncol = D_ncol[i];
                    int Di_nrow_128KB = (128 * 1024) / (sizeof(DTYPE) * Di_ncol);
                    int Di_blk_npt = Di_nrow_128KB / krnl_dim;
                    Di_nrow_128KB = Di_blk_npt * krnl_dim;
                    H2P_dense_mat_resize(Di, Di_nrow_128KB, Di_ncol);

                    int Di_npt_row = node_npt;
                    for (int blk_pt_s = 0; blk_pt_s < Di_npt_row; blk_pt_s += Di_blk_npt)
                    {
                        int blk_npt = (blk_pt_s + Di_blk_npt) > Di_npt_row ? Di_npt_row - blk_pt_s : Di_blk_npt;
                        int blk_srow = blk_pt_s * krnl_dim;
                        int blk_nrow = blk_npt * krnl_dim;
                        krnl_eval(
                            coord + pt_s + blk_pt_s, n_point, blk_npt,
                            coord + pt_s, n_point, node_npt,
                            Di->data, Di_ncol
                        );
                        CBLAS_GEMV(
                            CblasRowMajor, CblasNoTrans, blk_nrow, Di_ncol,
                            1.0, Di->data, Di_ncol, 
                            x_spos, 1, 1.0, y_spos + blk_srow, 1
                        );
                    }
                }
            }
        }  // End of i_blk0 loop 
        
        // 2. Off-diagonal blocks from inadmissible pairs matvec
        #pragma omp for schedule(dynamic) nowait
        for (int i_blk1 = 0; i_blk1 < n_D1_blk; i_blk1++)
        {
            int D_blk1_s = D_blk1->data[i_blk1];
            int D_blk1_e = D_blk1->data[i_blk1 + 1];
            for (int i = D_blk1_s; i < D_blk1_e; i++)
            {
                int node0  = r_inadm_pairs[2 * i];
                int node1  = r_inadm_pairs[2 * i + 1];
                int pt_s0  = pt_cluster[2 * node0];
                int pt_s1  = pt_cluster[2 * node1];
                int vec_s0 = mat_cluster[2 * node0];
                int vec_s1 = mat_cluster[2 * node1];
                int node0_npt = pt_cluster[2 * node0 + 1] - pt_s0 + 1;
                int node1_npt = pt_cluster[2 * node1 + 1] - pt_s1 + 1;
                DTYPE       *y_spos0 = y + vec_s0;
                DTYPE       *y_spos1 = y + vec_s1;
                const DTYPE *x_spos0 = x + vec_s0;
                const DTYPE *x_spos1 = x + vec_s1;
                
                if (krnl_matvec != NULL)
                {
                    H2P_ext_symm_krnl_matvec(
                        coord + pt_s0, n_point, node0_npt,
                        coord + pt_s1, n_point, node1_npt,
                        x_spos1, x_spos0, y_spos0, y_spos1,
                        pt_dim, krnl_dim, workbuf, krnl_matvec
                    );
                } else {
                    int Di_nrow = D_nrow[n_leaf_node + i];
                    int Di_ncol = D_ncol[n_leaf_node + i];
                    int Di_nrow_128KB = (128 * 1024) / (sizeof(DTYPE) * Di_ncol);
                    int Di_blk_npt = Di_nrow_128KB / krnl_dim;
                    Di_nrow_128KB = Di_blk_npt * krnl_dim;
                    H2P_dense_mat_resize(Di, Di_nrow_128KB, Di_ncol);
                    
                    int Di_npt_row = node0_npt;
                    for (int blk_pt_s = 0; blk_pt_s < Di_npt_row; blk_pt_s += Di_blk_npt)
                    {
                        int blk_npt = (blk_pt_s + Di_blk_npt) > Di_npt_row ? Di_npt_row - blk_pt_s : Di_blk_npt;
                        int blk_srow = blk_pt_s * krnl_dim;
                        int blk_nrow = blk_npt * krnl_dim;
                        krnl_eval(
                            coord + pt_s0 + blk_pt_s, n_point, blk_npt,
                            coord + pt_s1, n_point, node1_npt,
                            Di->data, Di_ncol
                        );
                        CBLAS_GESYMMV(
                            blk_nrow, Di_ncol, Di->data, Di_ncol,
                            x_spos1, x_spos0 + blk_srow, y_spos0 + blk_srow, y_spos1
                        );
                    }
                }
            }
        }  // End of i_blk1 loop 
        h2pack->tb[tid]->timer += H2P_get_wtime_sec();
    }  // End of "pragma omp parallel"
    
    #ifdef PROFILING_OUTPUT
    double max_t = 0.0, avg_t = 0.0, min_t = 1145141919.0;
    for (int i = 0; i < h2pack->n_thread; i++)
    {
        double thread_i_timer = h2pack->tb[i]->timer;
        avg_t += thread_i_timer;
        max_t = MAX(max_t, thread_i_timer);
        min_t = MIN(min_t, thread_i_timer);
    }
    avg_t /= (double) h2pack->n_thread;
    printf("[PROFILING] MatVec dense block sweep: min/avg/max thread wall-time = %.3lf, %.3lf, %.3lf (s)\n", min_t, avg_t, max_t);
    #endif
}

// H2 representation multiplies a column vector
void H2P_matvec(H2Pack_t h2pack, const DTYPE *x, DTYPE *y)
{
    double st, et;
    int krnl_mat_size = h2pack->krnl_mat_size;
    int n_thread = h2pack->n_thread;
    int BD_JIT = h2pack->BD_JIT;
    
    // 1. Reset partial y result in each thread-local buffer to 0
    st = H2P_get_wtime_sec();
    #pragma omp parallel num_threads(n_thread)
    {
        int tid = omp_get_thread_num();
        DTYPE *tid_y = h2pack->tb[tid]->y;
        memset(tid_y, 0, sizeof(DTYPE) * krnl_mat_size);
    }
    et = H2P_get_wtime_sec();
    h2pack->timers[8] += et - st;
    
    // 2. Upward sweep, calculate U_j^T * x_j
    st = H2P_get_wtime_sec();
    H2P_matvec_upward_sweep(h2pack, x);
    et = H2P_get_wtime_sec();
    h2pack->timers[4] += et - st;
    
    // 3. Intermediate sweep, calculate B_{ij} * (U_j^T * x_j)
    st = H2P_get_wtime_sec();
    if (BD_JIT == 1)
    {
        H2P_matvec_intermediate_sweep_JIT(h2pack, x);
    } else {
        H2P_matvec_intermediate_sweep_AOT(h2pack, x);
    }
    et = H2P_get_wtime_sec();
    h2pack->timers[5] += et - st;
    
    // 4. Downward sweep, calculate U_i * (B_{ij} * (U_j^T * x_j))
    st = H2P_get_wtime_sec();
    H2P_matvec_downward_sweep(h2pack, x);
    et = H2P_get_wtime_sec();
    h2pack->timers[6] += et - st;
    
    // 5. Dense blocks, calculate D_i * x_i
    st = H2P_get_wtime_sec();
    if (BD_JIT == 1)
    {
        H2P_matvec_dense_blocks_JIT(h2pack, x);
    } else {
        H2P_matvec_dense_blocks_AOT(h2pack, x);
    }
    et = H2P_get_wtime_sec();
    h2pack->timers[7] += et - st;
    
    // 6. Reduce sum partial y results
    st = H2P_get_wtime_sec();
    #pragma omp parallel num_threads(n_thread)
    {
        int tid = omp_get_thread_num();
        int spos, len;
        H2P_block_partition(krnl_mat_size, n_thread, tid, &spos, &len);
        
        DTYPE *y_src = h2pack->tb[0]->y;
        memcpy(y + spos, y_src + spos, sizeof(DTYPE) * len);
        for (int tid = 1; tid < n_thread; tid++)
        {
            y_src = h2pack->tb[tid]->y;
            #pragma omp simd
            for (int i = spos; i < spos + len; i++) y[i] += y_src[i];
        }
    }
    et = H2P_get_wtime_sec();
    h2pack->mat_size[7] = (n_thread + 1) * h2pack->krnl_mat_size;
    h2pack->timers[8] += et - st;
    
    h2pack->n_matvec++;
}

