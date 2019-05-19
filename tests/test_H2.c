#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <time.h>

#include <mkl.h>

#include "H2Pack.h"

// Symmetry kernel function: reciprocal
// Input parameters:
//   dim  : Dimension of point coordinate
//   x, y : Coordinate of two points
// Output parameter:
//   <return> : Output of kernel function
DTYPE reciprocal_kernel(const int dim, const DTYPE *x, const DTYPE *y)
{
    DTYPE res = 0.0;
    for (int i = 0; i < dim; i++)
    {
        DTYPE delta = x[i] - y[i];
        res += delta * delta;
    }
    if (res < 1e-20) res = 1.0;
    res = 1.0 / DSQRT(res);
    return res;
}

int main()
{
    int dim  = 2;
    int npts = 8000;
    DTYPE rel_tol = 1e-6;
    printf("dim: ");
    scanf("%d", &dim);
    printf("npts: ");
    scanf("%d", &npts);
    printf("rel_tol: ");
    scanf("%lf", &rel_tol);
    
    int max_child = 1 << dim;
    int max_leaf_points = 100;
    const DTYPE max_leaf_size = 0.0;
    
    FILE *inf, *ouf;
    double st, et, ut, total_t;
    
    srand(time(NULL));
    mkl_set_num_threads(1);
    
    DTYPE *coord = (DTYPE*) H2P_malloc_aligned(sizeof(DTYPE) * npts * dim);
    
    DTYPE k = pow((DTYPE) npts, 1.0 / (DTYPE) dim);
    for (int i = 0; i < npts; i++)
    {
        DTYPE *coord_i = coord + i * dim;
        for (int j = 0; j < dim; j++)
            coord_i[j] = k * (DTYPE) rand() / (DTYPE) RAND_MAX;
        /*
        DTYPE tmp = 0.0;
        tmp += coord_i[0] * coord_i[0];
        tmp += coord_i[1] * coord_i[1];
        tmp += coord_i[2] * coord_i[2];
        tmp = DSQRT(tmp);
        coord_i[0] /= tmp;
        coord_i[1] /= tmp;
        coord_i[2] /= tmp;
        */
    }
    ouf = fopen("coord.txt", "w");
    for (int i = 0; i < npts; i++)
    {
        DTYPE *coord_i = coord + i * dim;
        for (int j = 0; j < dim-1; j++) 
            fprintf(ouf, "% .15lf, ", coord_i[j]);
        fprintf(ouf, "% .15lf\n", coord_i[dim-1]);
    }
    fclose(ouf);
    /*
    inf = fopen("coord.txt", "r");
    for (int i = 0; i < npts; i++)
    {
        DTYPE *coord_i = coord + i * dim;
        for (int j = 0; j < dim-1; j++) 
            fscanf(inf, "%lf, ", &coord_i[j]);
        fscanf(inf, "%lf\n", &coord_i[dim-1]);
    }
    fclose(inf);
    */
    
    
    H2Pack_t h2pack;
    H2P_init(&h2pack, dim, QR_REL_NRM, &rel_tol);
    
    H2P_partition_points(h2pack, npts, coord, max_leaf_points, max_leaf_size);
    printf("H2Pack partition points done, used time = %e (s)\n", h2pack->timers[0]);
    printf(
        "n_node, n_leaf_node, max_child, max_level = %d %d %d %d\n", 
        h2pack->n_node, h2pack->n_leaf_node, h2pack->max_child, h2pack->max_level+1
    );
    printf(
        "n_r_adm_pair, n_r_inadm_pair, n_leaf_node = %d, %d, %d\n", 
        h2pack->n_r_adm_pair, h2pack->n_r_inadm_pair, h2pack->n_leaf_node
    );
    /*
    ouf = fopen("cluster.txt", "w");
    for (int i = 0; i < h2pack->n_node; i++)
        fprintf(ouf, "%d, %d\n", h2pack->cluster[2*i], h2pack->cluster[2*i+1]);
    fclose(ouf);

    ouf = fopen("admpair.txt", "w");
    for (int i = 0; i < h2pack->n_r_adm_pair; i++)
        fprintf(ouf, "%d, %d\n", h2pack->r_adm_pairs[2*i], h2pack->r_adm_pairs[2*i+1]);
    fclose(ouf);
    */

    kernel_func_ptr kernel = reciprocal_kernel;
    H2P_dense_mat_t *pp;
    DTYPE max_L = h2pack->enbox[h2pack->root_idx * 2 * dim + dim];
    st = H2P_get_wtime_sec();
    H2P_generate_proxy_point(dim, h2pack->max_level, 2, max_L, kernel, &pp);
    et = H2P_get_wtime_sec();
    printf("H2Pack generate proxy point used %.3lf (s)\n", et - st);
    H2P_build(h2pack, kernel, pp);
    double storage_k = 0.0;
    storage_k += (double) h2pack->mat_size[0];
    storage_k += (double) h2pack->mat_size[1];
    storage_k += (double) h2pack->mat_size[2];
    storage_k /= (double) npts;
    total_t = h2pack->timers[1] + h2pack->timers[2] + h2pack->timers[3] + h2pack->timers[4];
    printf(
        "H2P_build done, U, B, D, total time = %.3lf, %.3lf, %.3lf, %.3lf (s)\n",
        h2pack->timers[1], h2pack->timers[2], h2pack->timers[3], total_t
    );
    printf(
        "H2Pack U, B, D size = %d, %d, %d, size(U + B + D) / npts = %.2lf\n", 
        h2pack->mat_size[0], h2pack->mat_size[1], h2pack->mat_size[2], storage_k
    );
    /*
    ouf = fopen("Usize.txt", "w");
    for (int i = 0; i < h2pack->n_UJ; i++)
        fprintf(ouf, "%d, %d\n", h2pack->U[i]->nrow, h2pack->U[i]->ncol);
    fclose(ouf);
    
    ouf = fopen("Bsize.txt", "w");
    for (int i = 0; i < h2pack->n_B; i++)
        fprintf(ouf, "%d, %d\n", h2pack->B[i]->nrow, h2pack->B[i]->ncol);
    fclose(ouf);
    */

    DTYPE *x, *y0, *y1, *A;
    x  = (DTYPE*) H2P_malloc_aligned(sizeof(DTYPE) * npts);
    y0 = (DTYPE*) H2P_malloc_aligned(sizeof(DTYPE) * npts);
    y1 = (DTYPE*) H2P_malloc_aligned(sizeof(DTYPE) * npts);
    A  = (DTYPE*) H2P_malloc_aligned(sizeof(DTYPE) * npts * npts);
    assert(x != NULL && y0 != NULL && y1 != NULL && A != NULL);
    for (int i = 0; i < npts; i++) 
        x[i] = (DTYPE) rand() / (DTYPE) RAND_MAX;
    
    st = H2P_get_wtime_sec();
    for (int i = 0; i < npts; i++)
    {
        // Use the sorted coordinates
        DTYPE *coord_i = h2pack->coord + i * dim;
        for (int j = 0; j <= i; j++)
        {
            DTYPE *coord_j = h2pack->coord + j * dim;
            DTYPE res = reciprocal_kernel(dim, coord_i, coord_j);
            A[i * npts + j] = res;
            A[j * npts + i] = res;
        }
    }
    et = H2P_get_wtime_sec();
    printf("Dense matrix A construction time = %.4lf (s)\n", et - st);
    
    // Warm up
    CBLAS_GEMV(
        CblasRowMajor, CblasNoTrans, npts, npts, 
        1.0, A, npts, x, 1, 0.0, y0, 1
    );
    H2P_matvec(h2pack, x, y1); 
    h2pack->n_matvec = 0;
    memset(h2pack->timers + 4, 0, sizeof(double) * 4);
    
    ut = 0.0;
    for (int i = 0; i < 10; i++) 
    {
        st = H2P_get_wtime_sec();
        CBLAS_GEMV(
            CblasRowMajor, CblasNoTrans, npts, npts, 
            1.0, A, npts, x, 1, 0.0, y0, 1
        );
        et = H2P_get_wtime_sec();
        ut += et - st;
        
        H2P_matvec(h2pack, x, y1);
    }
    total_t = 0.0;
    for (int i = 4; i < 8; i++) 
    {
        h2pack->timers[i] /= 10.0;
        total_t += h2pack->timers[i];
    }
    printf(
        "H2P_matvec: up, mid, down, dense, total time = %.4lf, %.4lf, %.4lf, %.4lf, %.4lf (s)\n", 
        h2pack->timers[4], h2pack->timers[5], h2pack->timers[6], h2pack->timers[7], total_t
    );
    printf("cblas_dgemv time = %e (s)\n", ut / 10.0);
    
    DTYPE y0_norm = 0.0, err_norm = 0.0;
    for (int i = 0; i < npts; i++)
    {
        DTYPE diff = y1[i] - y0[i];
        y0_norm  += y0[i] * y0[i];
        err_norm += diff * diff;
    }
    y0_norm  = DSQRT(y0_norm);
    err_norm = DSQRT(err_norm);
    printf("||y_{H2} - y||_2 / ||y||_2 = %e\n", err_norm / y0_norm);

    H2P_free_aligned(x);
    H2P_free_aligned(y0);
    H2P_free_aligned(y1);
    H2P_free_aligned(A);
    H2P_free_aligned(coord);
    H2P_destroy(h2pack);
}
