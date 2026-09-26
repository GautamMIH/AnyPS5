#include <cmath>
#include <limits>

#include "prx/libc/include/General.hpp"

extern "C" {

double APS5_VABI cbrt_nid_postfix(double x) { return std::cbrt(x); }
double APS5_VABI asin_nid_postfix(double x) { return std::asin(x); }
double APS5_VABI acos_nid_postfix(double x) { return std::acos(x); }
double APS5_VABI exp_nid_postfix(double x) { return std::exp(x); }
double APS5_VABI atan_nid_postfix(double x) { return std::atan(x); }
double APS5_VABI tan_nid_postfix(double x) { return std::tan(x); }
double APS5_VABI log2_nid_postfix(double x) { return std::log2(x); }
double APS5_VABI log_nid_postfix(double x) { return std::log(x); }

float APS5_VABI sinf_nid_postfix(float x) { return std::sin(x); }
float APS5_VABI cosf_nid_postfix(float x) { return std::cos(x); }

void APS5_VABI sincosf_nid_postfix(float x, float* sinp, float* cosp) {
    *sinp = std::sin(x);
    *cosp = std::cos(x);
}

double APS5_VABI sin_nid_postfix(double x) { return std::sin(x); }
double APS5_VABI cos_nid_postfix(double x) { return std::cos(x); }

void APS5_VABI sincos_nid_postfix(double x, double* sinp, double* cosp) {
    *sinp = std::sin(x);
    *cosp = std::cos(x);
}

float APS5_VABI atanf_nid_postfix(float x) { return std::atan(x); }
double APS5_VABI atan2_nid_postfix(double y, double x) { return std::atan2(y, x); }
float APS5_VABI powf_nid_postfix(float base, float exp) { return std::pow(base, exp); }
double APS5_VABI pow_nid_postfix(double base, double exp) { return std::pow(base, exp); }
float APS5_VABI expf_nid_postfix(float x) { return std::exp(x); }
float APS5_VABI exp2f_nid_postfix(float x) { return std::exp2(x); }
float APS5_VABI logf_nid_postfix(float x) { return std::log(x); }
float APS5_VABI log2f_nid_postfix(float x) { return std::log2(x); }
double APS5_VABI log10_nid_postfix(double x) { return std::log10(x); }
float APS5_VABI ldexpf_nid_postfix(float x, int exp) { return std::ldexp(x, exp); }
double APS5_VABI fmod_nid_postfix(double x, double y) { return std::fmod(x, y); }
float APS5_VABI roundf_nid_postfix(float x) { return std::round(x); }
double APS5_VABI round_nid_postfix(double x) { return std::round(x); }

int APS5_VABI __isfinite_nid_postfix(double x) { return std::isfinite(x) ? 1 : 0; }
int APS5_VABI __isnan_nid_postfix(double x) { return std::isnan(x) ? 1 : 0; }
int APS5_VABI __signbit_nid_postfix(double x) { return std::signbit(x) ? 1 : 0; }

double APS5_VABI modf_nid_postfix(double x, double* integral) { return std::modf(x, integral); }
float APS5_VABI modff_nid_postfix(float x, float* integral) { return std::modf(x, integral); }
float APS5_VABI fmodf_nid_postfix(float x, float y) { return std::fmod(x, y); }
float APS5_VABI atan2f_nid_postfix(float y, float x) { return std::atan2(y, x); }
float APS5_VABI asinf_nid_postfix(float x) { return std::asin(x); }
float APS5_VABI acosf_nid_postfix(float x) { return std::acos(x); }
float APS5_VABI tanf_nid_postfix(float x) { return std::tan(x); }
double APS5_VABI ldexp_nid_postfix(double x, int exp) { return std::ldexp(x, exp); }
double APS5_VABI exp2_nid_postfix(double x) { return std::exp2(x); }
float APS5_VABI log10f_nid_postfix(float x) { return std::log10(x); }
float APS5_VABI hypotf_nid_postfix(float x, float y) { return std::hypot(x, y); }
double APS5_VABI hypot_nid_postfix(double x, double y) { return std::hypot(x, y); }
double APS5_VABI frexp_nid_postfix(double x, int* exp) { return std::frexp(x, exp); }
long double APS5_VABI frexpl_nid_postfix(long double x, int* exp) { return std::frexp(x, exp); }
double APS5_VABI scalbn_nid_postfix(double x, int exp) { return std::scalbn(x, exp); }
double APS5_VABI logb_nid_postfix(double x) { return std::logb(x); }
float APS5_VABI logbf_nid_postfix(float x) { return std::logb(x); }
float APS5_VABI nearbyintf_nid_postfix(float x) { return std::nearbyint(x); }
float APS5_VABI fmaxf_nid_postfix(float x, float y) { return std::fmax(x, y); }
float APS5_VABI fminf_nid_postfix(float x, float y) { return std::fmin(x, y); }

int APS5_VABI __isfinitef_nid_postfix(float x) { return std::isfinite(x) ? 1 : 0; }
int APS5_VABI __isfinitel_nid_postfix(long double x) { return std::isfinite(x) ? 1 : 0; }
int APS5_VABI __isinf_nid_postfix(double x) { return std::isinf(x) ? 1 : 0; }
int APS5_VABI __isinff_nid_postfix(float x) { return std::isinf(x) ? 1 : 0; }
int APS5_VABI __isnanf_nid_postfix(float x) { return std::isnan(x) ? 1 : 0; }

short APS5_VABI _Dtest_nid_postfix(double* value) {
    constexpr short kDenormal = -2;
    constexpr short kFinite = -1;
    constexpr short kInfinite = 1;
    constexpr short kNotANumber = 2;
    switch (std::fpclassify(*value)) {
    case FP_NAN: return kNotANumber;
    case FP_INFINITE: return kInfinite;
    case FP_SUBNORMAL: return kDenormal;
    case FP_ZERO: return 0;
    default: return kFinite;
    }
}

float APS5_VABI _FSinh_nid_postfix(float x, float y) { return y * std::sinh(x); }

float APS5_VABI __powisf2_nid_postfix(float base, int exponent) {
    float result = 1.0f;
    const bool negative = exponent < 0;
    unsigned int remaining = negative ? 0u - static_cast<unsigned int>(exponent) : static_cast<unsigned int>(exponent);
    for (float factor = base; remaining != 0; remaining >>= 1, factor *= factor)
        if (remaining & 1u) result *= factor;
    return negative ? 1.0f / result : result;
}

double APS5_VABI __powidf2_nid_postfix(double base, int exponent) {
    double result = 1.0;
    const bool negative = exponent < 0;
    unsigned int remaining = negative ? 0u - static_cast<unsigned int>(exponent) : static_cast<unsigned int>(exponent);
    for (double factor = base; remaining != 0; remaining >>= 1, factor *= factor)
        if (remaining & 1u) result *= factor;
    return negative ? 1.0 / result : result;
}

unsigned __int128 APS5_VABI __udivti3_nid_postfix(unsigned __int128 dividend, unsigned __int128 divisor) { return dividend / divisor; }

union DinkumwareConstant {
    unsigned short Words[8];
    float Float;
    double Double;
    long double LongDouble;
};

DinkumwareConstant _Inf_nid_postfix = {.Double = std::numeric_limits<double>::infinity()};

}
