#ifndef __H2PACK_2D_KERNELS_H__
#define __H2PACK_2D_KERNELS_H__

#include <math.h>

#include "H2Pack_config.h"
#include "x86_intrin_wrapper.h" 

#ifndef KRNL_EVAL_PARAM 
#define KRNL_EVAL_PARAM \
    const DTYPE *coord0, const int ld0, const int n0, \
    const DTYPE *coord1, const int ld1, const int n1, \
    const void *param, DTYPE *mat, const int ldm 
#endif

#ifndef KRNL_BIMV_PARAM
#define KRNL_BIMV_PARAM \
    const DTYPE *coord0, const int ld0, const int n0,            \
    const DTYPE *coord1, const int ld1, const int n1,            \
    const void *param, const DTYPE *x_in_0, const DTYPE *x_in_1, \
    DTYPE *x_out_0, DTYPE *x_out_1
#endif

#define EXTRACT_2D_COORD() \
    const DTYPE *x0 = coord0 + ld0 * 0; \
    const DTYPE *y0 = coord0 + ld0 * 1; \
    const DTYPE *x1 = coord1 + ld1 * 0; \
    const DTYPE *y1 = coord1 + ld1 * 1; 

// When counting bimv flops, report effective flops (1 / sqrt(x) == 2 flops)
// instead of achieved flops (1 / sqrt(x) == 1 + NEWTON_ITER * 4 flops)

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================ //
// ====================   Coulomb Kernel   ==================== //
// ============================================================ //

const  int  Coulomb_2D_krnl_bimv_flop = 11;

static void Coulomb_2D_eval_intrin_d(KRNL_EVAL_PARAM)
{
    EXTRACT_2D_COORD();
    const int n1_vec = (n1 / SIMD_LEN) * SIMD_LEN;
    const vec_d frsqrt_pf = vec_frsqrt_pf_d();
    for (int i = 0; i < n0; i++)
    {
        DTYPE *mat_irow = mat + i * ldm;
        
        vec_d x0_iv = vec_bcast_d(x0 + i);
        vec_d y0_iv = vec_bcast_d(y0 + i);
        for (int j = 0; j < n1_vec; j += SIMD_LEN)
        {
            vec_d dx = vec_sub_d(x0_iv, vec_loadu_d(x1 + j));
            vec_d dy = vec_sub_d(y0_iv, vec_loadu_d(y1 + j));
            
            vec_d r2 = vec_mul_d(dx, dx);
            r2 = vec_fmadd_d(dy, dy, r2);
            
            vec_d rinv = vec_mul_d(frsqrt_pf, vec_frsqrt_d(r2));
            vec_storeu_d(mat_irow + j, rinv);
        }
        
        const DTYPE x0_i = x0[i];
        const DTYPE y0_i = y0[i];
        for (int j = n1_vec; j < n1; j++)
        {
            DTYPE dx = x0_i - x1[j];
            DTYPE dy = y0_i - y1[j];
            DTYPE r2 = dx * dx + dy * dy;
            mat_irow[j] = (r2 == 0.0) ? 0.0 : (1.0 / DSQRT(r2));
        }
    }
}

static void Coulomb_2D_krnl_bimv_intrin_d(KRNL_BIMV_PARAM)
{
    EXTRACT_2D_COORD();
    const vec_d frsqrt_pf = vec_frsqrt_pf_d();
    for (int i = 0; i < n0; i += 2)
    {
        vec_d sum_v0 = vec_zero_d();
        vec_d sum_v1 = vec_zero_d();
        const vec_d x0_i0v = vec_bcast_d(x0 + i);
        const vec_d y0_i0v = vec_bcast_d(y0 + i);
        const vec_d x0_i1v = vec_bcast_d(x0 + i + 1);
        const vec_d y0_i1v = vec_bcast_d(y0 + i + 1);
        const vec_d x_in_1_i0v = vec_bcast_d(x_in_1 + i);
        const vec_d x_in_1_i1v = vec_bcast_d(x_in_1 + i + 1);
        for (int j = 0; j < n1; j += SIMD_LEN)
        {
            vec_d d0, d1, jv, r20, r21;
            
            jv  = vec_load_d(x1 + j);
            d0  = vec_sub_d(x0_i0v, jv);
            d1  = vec_sub_d(x0_i1v, jv);
            r20 = vec_mul_d(d0, d0);
            r21 = vec_mul_d(d1, d1);
            
            jv  = vec_load_d(y1 + j);
            d0  = vec_sub_d(y0_i0v, jv);
            d1  = vec_sub_d(y0_i1v, jv);
            r20 = vec_fmadd_d(d0, d0, r20);
            r21 = vec_fmadd_d(d1, d1, r21);
            
            d0 = vec_load_d(x_in_0 + j);
            d1 = vec_load_d(x_out_1 + j);
            
            r20 = vec_mul_d(frsqrt_pf, vec_frsqrt_d(r20));
            r21 = vec_mul_d(frsqrt_pf, vec_frsqrt_d(r21));
            
            sum_v0 = vec_fmadd_d(d0, r20, sum_v0);
            sum_v1 = vec_fmadd_d(d0, r21, sum_v1);
            
            d1 = vec_fmadd_d(x_in_1_i0v, r20, d1);
            d1 = vec_fmadd_d(x_in_1_i1v, r21, d1);
            vec_store_d(x_out_1 + j, d1);
        }
        x_out_0[i]   += vec_reduce_add_d(sum_v0);
        x_out_0[i+1] += vec_reduce_add_d(sum_v1);
    }
}

// ============================================================ //
// ==================   Erf Coulomb Kernel   ================== //
// ============================================================ //

const  int  ECoulomb_2D_krnl_bimv_flop = 15;

static void ECoulomb_2D_eval_intrin_d(KRNL_EVAL_PARAM)
{
    EXTRACT_2D_COORD();
    const int n1_vec = (n1 / SIMD_LEN) * SIMD_LEN;
    const vec_d frsqrt_pf = vec_frsqrt_pf_d();
    const vec_d v_05 = vec_set1_d(0.5);
    for (int i = 0; i < n0; i++)
    {
        DTYPE *mat_irow = mat + i * ldm;
        
        vec_d x0_iv = vec_bcast_d(x0 + i);
        vec_d y0_iv = vec_bcast_d(y0 + i);
        for (int j = 0; j < n1_vec; j += SIMD_LEN)
        {
            vec_d dx = vec_sub_d(x0_iv, vec_loadu_d(x1 + j));
            vec_d dy = vec_sub_d(y0_iv, vec_loadu_d(y1 + j));
            
            vec_d r2 = vec_mul_d(dx, dx);
            r2 = vec_fmadd_d(dy, dy, r2);
            vec_d semi_r = vec_mul_d(v_05, vec_sqrt_d(r2));
            
            vec_d rinv = vec_mul_d(frsqrt_pf, vec_frsqrt_d(r2));
            vec_d val  = vec_mul_d(rinv, vec_erf_d(semi_r));
            vec_storeu_d(mat_irow + j, val);
        }
        
        const DTYPE x0_i = x0[i];
        const DTYPE y0_i = y0[i];
        for (int j = n1_vec; j < n1; j++)
        {
            DTYPE dx = x0_i - x1[j];
            DTYPE dy = y0_i - y1[j];
            DTYPE r2 = dx * dx + dy * dy;
            DTYPE r  = DSQRT(r2);
            DTYPE invr = (r2 == 0.0) ? 0.0 : (1.0 / r);
            mat_irow[j] = invr * DERF(0.5 * r);
        }
    }
}

static void ECoulomb_2D_krnl_bimv_intrin_d(KRNL_BIMV_PARAM)
{
    EXTRACT_2D_COORD();
    const vec_d frsqrt_pf = vec_frsqrt_pf_d();
    const vec_d v_05 = vec_set1_d(0.5);
    for (int i = 0; i < n0; i += 2)
    {
        vec_d sum_v0 = vec_zero_d();
        vec_d sum_v1 = vec_zero_d();
        const vec_d x0_i0v = vec_bcast_d(x0 + i);
        const vec_d y0_i0v = vec_bcast_d(y0 + i);
        const vec_d x0_i1v = vec_bcast_d(x0 + i + 1);
        const vec_d y0_i1v = vec_bcast_d(y0 + i + 1);
        const vec_d x_in_1_i0v = vec_bcast_d(x_in_1 + i);
        const vec_d x_in_1_i1v = vec_bcast_d(x_in_1 + i + 1);
        for (int j = 0; j < n1; j += SIMD_LEN)
        {
            vec_d d0, d1, jv, r20, r21, semi_r0, semi_r1, val0, val1;
            
            jv  = vec_load_d(x1 + j);
            d0  = vec_sub_d(x0_i0v, jv);
            d1  = vec_sub_d(x0_i1v, jv);
            r20 = vec_mul_d(d0, d0);
            r21 = vec_mul_d(d1, d1);
            
            jv  = vec_load_d(y1 + j);
            d0  = vec_sub_d(y0_i0v, jv);
            d1  = vec_sub_d(y0_i1v, jv);
            r20 = vec_fmadd_d(d0, d0, r20);
            r21 = vec_fmadd_d(d1, d1, r21);
            
            semi_r0 = vec_mul_d(v_05, vec_sqrt_d(r20));
            semi_r1 = vec_mul_d(v_05, vec_sqrt_d(r21));

            d0 = vec_load_d(x_in_0 + j);
            d1 = vec_load_d(x_out_1 + j);
            
            r20 = vec_mul_d(frsqrt_pf, vec_frsqrt_d(r20));
            r21 = vec_mul_d(frsqrt_pf, vec_frsqrt_d(r21));
            
            val0 = vec_mul_d(r20, vec_erf_d(semi_r0));
            val1 = vec_mul_d(r21, vec_erf_d(semi_r1));

            sum_v0 = vec_fmadd_d(d0, val0, sum_v0);
            sum_v1 = vec_fmadd_d(d0, val1, sum_v1);
            
            d1 = vec_fmadd_d(x_in_1_i0v, val0, d1);
            d1 = vec_fmadd_d(x_in_1_i1v, val1, d1);
            vec_store_d(x_out_1 + j, d1);
        }
        x_out_0[i]   += vec_reduce_add_d(sum_v0);
        x_out_0[i+1] += vec_reduce_add_d(sum_v1);
    }
}

// ============================================================ //
// ====================   Gaussian Kernel   =================== //
// ============================================================ //

const  int  Gaussian_2D_krnl_bimv_flop = 11;

static void Gaussian_2D_eval_intrin_d(KRNL_EVAL_PARAM)
{
    EXTRACT_2D_COORD();
    const int n1_vec = (n1 / SIMD_LEN) * SIMD_LEN;
    const DTYPE *param_ = (DTYPE*) param;
    const DTYPE l = param_[0];
    const vec_d l_v = vec_set1_d(l);
    for (int i = 0; i < n0; i++)
    {
        DTYPE *mat_irow = mat + i * ldm;
        
        vec_d x0_iv = vec_bcast_d(x0 + i);
        vec_d y0_iv = vec_bcast_d(y0 + i);
        for (int j = 0; j < n1_vec; j += SIMD_LEN)
        {
            vec_d dx = vec_sub_d(x0_iv, vec_loadu_d(x1 + j));
            vec_d dy = vec_sub_d(y0_iv, vec_loadu_d(y1 + j));
            
            vec_d r2 = vec_mul_d(dx, dx);
            r2 = vec_fmadd_d(dy, dy, r2);
            
            r2 = vec_fnmadd_d(r2, l_v, vec_zero_d());
            r2 = vec_exp_d(r2);
            
            vec_storeu_d(mat_irow + j, r2);
        }
        
        const DTYPE x0_i = x0[i];
        const DTYPE y0_i = y0[i];
        for (int j = n1_vec; j < n1; j++)
        {
            DTYPE dx = x0_i - x1[j];
            DTYPE dy = y0_i - y1[j];
            DTYPE r2 = dx * dx + dy * dy;
            mat_irow[j] = exp(-l * r2);
        }
    }
}

static void Gaussian_2D_krnl_bimv_intrin_d(KRNL_BIMV_PARAM)
{
    EXTRACT_2D_COORD();
    const DTYPE *param_ = (DTYPE*) param;
    const DTYPE l = param_[0];
    const vec_d l_v = vec_set1_d(l);
    for (int i = 0; i < n0; i += 2)
    {
        vec_d sum_v0 = vec_zero_d();
        vec_d sum_v1 = vec_zero_d();
        const vec_d x0_i0v = vec_bcast_d(x0 + i);
        const vec_d y0_i0v = vec_bcast_d(y0 + i);
        const vec_d x0_i1v = vec_bcast_d(x0 + i + 1);
        const vec_d y0_i1v = vec_bcast_d(y0 + i + 1);
        const vec_d x_in_1_i0v = vec_bcast_d(x_in_1 + i);
        const vec_d x_in_1_i1v = vec_bcast_d(x_in_1 + i + 1);
        for (int j = 0; j < n1; j += SIMD_LEN)
        {
            vec_d d0, d1, jv, r20, r21;
            
            jv  = vec_load_d(x1 + j);
            d0  = vec_sub_d(x0_i0v, jv);
            d1  = vec_sub_d(x0_i1v, jv);
            r20 = vec_mul_d(d0, d0);
            r21 = vec_mul_d(d1, d1);
            
            jv  = vec_load_d(y1 + j);
            d0  = vec_sub_d(y0_i0v, jv);
            d1  = vec_sub_d(y0_i1v, jv);
            r20 = vec_fmadd_d(d0, d0, r20);
            r21 = vec_fmadd_d(d1, d1, r21);
            
            d0 = vec_load_d(x_in_0 + j);
            d1 = vec_load_d(x_out_1 + j);
            
            r20 = vec_fnmadd_d(r20, l_v, vec_zero_d());
            r21 = vec_fnmadd_d(r21, l_v, vec_zero_d());
            r20 = vec_exp_d(r20);
            r21 = vec_exp_d(r21);
            
            sum_v0 = vec_fmadd_d(d0, r20, sum_v0);
            sum_v1 = vec_fmadd_d(d0, r21, sum_v1);
            
            d1 = vec_fmadd_d(x_in_1_i0v, r20, d1);
            d1 = vec_fmadd_d(x_in_1_i1v, r21, d1);
            vec_store_d(x_out_1 + j, d1);
        }
        x_out_0[i]   += vec_reduce_add_d(sum_v0);
        x_out_0[i+1] += vec_reduce_add_d(sum_v1);
    }
}

// ============================================================ //
// =====================   Matern Kernel   ==================== //
// ============================================================ //

const  int  Matern_2D_krnl_bimv_flop = 14;

#define NSQRT3 -1.7320508075688772

static void Matern_2D_eval_intrin_d(KRNL_EVAL_PARAM)
{
    EXTRACT_2D_COORD();
    const int n1_vec = (n1 / SIMD_LEN) * SIMD_LEN;
    const DTYPE *param_ = (DTYPE*) param;
    const DTYPE nsqrt3_l = NSQRT3 * param_[0];
    const vec_d nsqrt3_l_v = vec_set1_d(nsqrt3_l);
    const vec_d v_1 = vec_set1_d(1.0);
    for (int i = 0; i < n0; i++)
    {
        DTYPE *mat_irow = mat + i * ldm;
        
        vec_d x0_iv = vec_bcast_d(x0 + i);
        vec_d y0_iv = vec_bcast_d(y0 + i);
        for (int j = 0; j < n1_vec; j += SIMD_LEN)
        {
            vec_d dx = vec_sub_d(x0_iv, vec_loadu_d(x1 + j));
            vec_d dy = vec_sub_d(y0_iv, vec_loadu_d(y1 + j));
            
            vec_d r = vec_mul_d(dx, dx);
            r = vec_fmadd_d(dy, dy, r);
            r = vec_sqrt_d(r);
            r = vec_mul_d(r, nsqrt3_l_v);
            r = vec_mul_d(vec_sub_d(v_1, r), vec_exp_d(r));
            
            vec_storeu_d(mat_irow + j, r);
        }
        
        const DTYPE x0_i = x0[i];
        const DTYPE y0_i = y0[i];
        for (int j = n1_vec; j < n1; j++)
        {
            DTYPE dx = x0_i - x1[j];
            DTYPE dy = y0_i - y1[j];
            DTYPE r  = sqrt(dx * dx + dy * dy);
            r = r * nsqrt3_l;
            r = (1.0 - r) * exp(r);
            mat_irow[j] = r;
        }
    }
}

static void Matern_2D_krnl_bimv_intrin_d(KRNL_BIMV_PARAM)
{
    EXTRACT_2D_COORD();
    const DTYPE *param_ = (DTYPE*) param;
    const DTYPE nsqrt3_l = NSQRT3 * param_[0];
    const vec_d nsqrt3_l_v = vec_set1_d(nsqrt3_l);
    const vec_d v_1 = vec_set1_d(1.0);
    for (int i = 0; i < n0; i += 2)
    {
        vec_d sum_v0 = vec_zero_d();
        vec_d sum_v1 = vec_zero_d();
        const vec_d x0_i0v = vec_bcast_d(x0 + i);
        const vec_d y0_i0v = vec_bcast_d(y0 + i);
        const vec_d x0_i1v = vec_bcast_d(x0 + i + 1);
        const vec_d y0_i1v = vec_bcast_d(y0 + i + 1);
        const vec_d x_in_1_i0v = vec_bcast_d(x_in_1 + i);
        const vec_d x_in_1_i1v = vec_bcast_d(x_in_1 + i + 1);
        for (int j = 0; j < n1; j += SIMD_LEN)
        {
            vec_d d0, d1, jv, r0, r1;
            
            jv = vec_load_d(x1 + j);
            d0 = vec_sub_d(x0_i0v, jv);
            d1 = vec_sub_d(x0_i1v, jv);
            r0 = vec_mul_d(d0, d0);
            r1 = vec_mul_d(d1, d1);
            
            jv = vec_load_d(y1 + j);
            d0 = vec_sub_d(y0_i0v, jv);
            d1 = vec_sub_d(y0_i1v, jv);
            r0 = vec_fmadd_d(d0, d0, r0);
            r1 = vec_fmadd_d(d1, d1, r1);
            
            r0 = vec_sqrt_d(r0);
            r1 = vec_sqrt_d(r1);
            
            d0 = vec_load_d(x_in_0 + j);
            d1 = vec_load_d(x_out_1 + j);
            
            r0 = vec_mul_d(r0, nsqrt3_l_v);
            r1 = vec_mul_d(r1, nsqrt3_l_v);
            r0 = vec_mul_d(vec_sub_d(v_1, r0), vec_exp_d(r0));
            r1 = vec_mul_d(vec_sub_d(v_1, r1), vec_exp_d(r1));
            
            sum_v0 = vec_fmadd_d(d0, r0, sum_v0);
            sum_v1 = vec_fmadd_d(d0, r1, sum_v1);
            
            d1 = vec_fmadd_d(x_in_1_i0v, r0, d1);
            d1 = vec_fmadd_d(x_in_1_i1v, r1, d1);
            vec_store_d(x_out_1 + j, d1);
        }
        x_out_0[i]   += vec_reduce_add_d(sum_v0);
        x_out_0[i+1] += vec_reduce_add_d(sum_v1);
    }
}

// ============================================================ //
// ===================   Quadratic Kernel   =================== //
// ============================================================ //

const  int  Quadratic_2D_krnl_bimv_flop = 12;

static void Quadratic_2D_eval_intrin_d(KRNL_EVAL_PARAM)
{
    EXTRACT_2D_COORD();
    const int n1_vec = (n1 / SIMD_LEN) * SIMD_LEN;
    const DTYPE *param_ = (DTYPE*) param;
    const DTYPE c = param_[0];
    const DTYPE a = param_[1];
    const vec_d vec_c = vec_set1_d(c);
    const vec_d vec_a = vec_set1_d(a);
    const vec_d vec_1 = vec_set1_d(1.0);
    for (int i = 0; i < n0; i++)
    {
        DTYPE *mat_irow = mat + i * ldm;
        
        vec_d x0_iv = vec_bcast_d(x0 + i);
        vec_d y0_iv = vec_bcast_d(y0 + i);
        for (int j = 0; j < n1_vec; j += SIMD_LEN)
        {
            vec_d dx = vec_sub_d(x0_iv, vec_loadu_d(x1 + j));
            vec_d dy = vec_sub_d(y0_iv, vec_loadu_d(y1 + j));
            
            vec_d r2 = vec_mul_d(dx, dx);
            r2 = vec_fmadd_d(dy, dy, r2);
            
            r2 = vec_fmadd_d(r2, vec_c, vec_1);
            r2 = vec_pow_d(r2, vec_a);
            
            vec_storeu_d(mat_irow + j, r2);
        }
        
        const DTYPE x0_i = x0[i];
        const DTYPE y0_i = y0[i];
        for (int j = n1_vec; j < n1; j++)
        {
            DTYPE dx = x0_i - x1[j];
            DTYPE dy = y0_i - y1[j];
            DTYPE r2 = dx * dx + dy * dy;

            r2 = 1.0 + c * r2;
            r2 = DPOW(r2, a);
            mat_irow[j] = r2;
        }
    }
}

static void Quadratic_2D_krnl_bimv_intrin_d(KRNL_BIMV_PARAM)
{
    EXTRACT_2D_COORD();
    const DTYPE *param_ = (DTYPE*) param;
    const vec_d vec_c = vec_bcast_d(param_ + 0);
    const vec_d vec_a = vec_bcast_d(param_ + 1);
    const vec_d vec_1 = vec_set1_d(1.0);
    for (int i = 0; i < n0; i += 2)
    {
        vec_d sum_v0 = vec_zero_d();
        vec_d sum_v1 = vec_zero_d();
        const vec_d x0_i0v = vec_bcast_d(x0 + i);
        const vec_d y0_i0v = vec_bcast_d(y0 + i);
        const vec_d x0_i1v = vec_bcast_d(x0 + i + 1);
        const vec_d y0_i1v = vec_bcast_d(y0 + i + 1);
        const vec_d x_in_1_i0v = vec_bcast_d(x_in_1 + i);
        const vec_d x_in_1_i1v = vec_bcast_d(x_in_1 + i + 1);
        for (int j = 0; j < n1; j += SIMD_LEN)
        {
            vec_d d0, d1, jv, r20, r21;
            
            jv  = vec_load_d(x1 + j);
            d0  = vec_sub_d(x0_i0v, jv);
            d1  = vec_sub_d(x0_i1v, jv);
            r20 = vec_mul_d(d0, d0);
            r21 = vec_mul_d(d1, d1);
            
            jv  = vec_load_d(y1 + j);
            d0  = vec_sub_d(y0_i0v, jv);
            d1  = vec_sub_d(y0_i1v, jv);
            r20 = vec_fmadd_d(d0, d0, r20);
            r21 = vec_fmadd_d(d1, d1, r21);
            
            d0 = vec_load_d(x_in_0 + j);
            d1 = vec_load_d(x_out_1 + j);
            
            r20 = vec_fmadd_d(r20, vec_c, vec_1);
            r21 = vec_fmadd_d(r21, vec_c, vec_1);

            r20 = vec_pow_d(r20, vec_a);
            r21 = vec_pow_d(r21, vec_a);
            
            sum_v0 = vec_fmadd_d(d0, r20, sum_v0);
            sum_v1 = vec_fmadd_d(d0, r21, sum_v1);
            
            d1 = vec_fmadd_d(x_in_1_i0v, r20, d1);
            d1 = vec_fmadd_d(x_in_1_i1v, r21, d1);
            vec_store_d(x_out_1 + j, d1);
        }
        x_out_0[i]   += vec_reduce_add_d(sum_v0);
        x_out_0[i+1] += vec_reduce_add_d(sum_v1);
    }
}

#ifdef __cplusplus
}
#endif

#endif