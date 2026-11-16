/* nlmath: the NAOMI graphics library's angle and floating point maths. An angle is 0 to 0xffff for a turn. */
#include "types.h"

#define fabs( x )	_builtin_fabsf( x )
#define sqrt( x )	_builtin_sqrtf( x )

#define FLT_MAX		3.40282347e+38f
#define LN_2		0.6931472f
#define LN_10		2.3025851f
#define TWO_PI		6.2831855f
#define HALF_PI		1.5707964f
/* half of pi in two parts, for a reduction that keeps more bits */
#define HALF_PI_HIGH	1.5708008f
#define HALF_PI_LOW	4.454455e-06f
/* from here up a float has no fraction */
#define NO_FRACTION	8388608.0f

Uint32 nl_random_next = 1;

Sint32 nlArcSin( Float x );
Sint32 nlArcTan( Float x );
Float nlSin( int ang );
Float nlSinf( Float x );
Float nlTan( int ang );
Float nlExp( Float x );
Float nlFloor( Float x );
Float nlLdexpf( Float x, int n );
Float nlLog( Float x );
Sint32 nlRand( void );
static Float sinf_sub( Float h, Sint32 *n );
static Float tanf_sub( Float x, Sint32 *n );

Sint32 nlArcCos( Float x )
{
	if( fabs( x ) > 1.0f )
		return 0;
	return ( 0x4000 - nlArcSin( x ) ) & 0xffff;
}

Sint32 nlArcCosec( Float x )
{
	if( x == 0.0f )
		return 0;
	return nlArcSin( 1.0f / x );
}

Sint32 nlArcCot( Float x )
{
	if( x == 0.0f )
		return 0;
	return nlArcTan( 1.0f / x );
}

Sint32 nlArcSec( Float x )
{
	if( x == 0.0f )
		return 0;
	return nlArcCos( 1.0f / x );
}

Sint32 nlArcSin( Float x )
{
	Float s;

	if( fabs( x ) > 1.0f )
		return 0;
	s = sqrt( 1.0f - x * x );
	if( s != 0.0f )
		return nlArcTan( x / s );
	if( x > 0.0f )
		return 0x4000;
	return 0xc000;
}

Float nlAbsSqrt( Float x )
{
	if( x < 0.0f )
		return -sqrt( -x );
	return sqrt( x );
}

/* the angle of a tangent, by a continued fraction of 13 terms; past 1 it is worked out from 1 / x */
Sint32 nlArcTan( Float x )
{
	Float a, t;
	Sint32 n;

	if( fabs( x ) <= 1.0f )
		a = x;
	else
		a = 1.0f / x;
	t = 0.0f;
	for( n = 13; n >= 1; n-- )
		t = (Float)n * (Float)n * a * a / ( (Float)( n * 2 ) + t + 1.0f );
	a = a / ( t + 1.0f );
	n = (Sint32)( ( a + TWO_PI ) * 65536.0f / TWO_PI + 0.5f ) & 0xffff;
	if( x > 1.0f )
		return ( 0x4000 - n ) & 0xffff;
	if( x < -1.0f )
		return ( 0xc000 - n ) & 0xffff;
	return n;
}

Sint32 nlArcTan2( Float y, Float x )
{
	if( x >= y ){
		if( x >= -y ){
			if( x == 0.0f )
				return 0;
			return nlArcTan( y / x );
		}
		if( y == 0.0f )
			return 0xc000;
		return ( 0xc000 - nlArcTan( x / y ) ) & 0xffff;
	}
	if( x >= -y ){
		if( y == 0.0f )
			return 0x4000;
		return ( 0x4000 - nlArcTan( x / y ) ) & 0xffff;
	}
	if( x == 0.0f )
		return 0x8000;
	return ( nlArcTan( y / x ) + 0x8000 ) & 0xffff;
}

Float nlCeil( Float x )
{
	Float y = x;	/* unused */

	return -nlFloor( -x );
}

Float nlCos( int ang )
{
	Sint32 a = (Uint16)ang;

	if( a > 0x8000 )
		a = 0x10000 - a;
	return nlSin( 0x4000 - a );
}

Float nlCosec( int ang )
{
	ang = (Uint16)ang;
	if( ang == 0 )
		return FLT_MAX;
	if( ang == 0x8000 )
		return FLT_MAX;
	return 1.0f / nlSin( ang );
}

Float nlCosf( Float x )
{
	Float y = x;	/* unused */

	return nlSinf( HALF_PI - fabs( x ) );
}

Float nlCosh( Uint16 ang )
{
	Float x = (Float)ang * TWO_PI / 65536.0f;
	Float e = nlExp( x );

	return ( 1.0f / e + e ) / 2.0f;
}

Float nlCot( int ang )
{
	ang = (Uint16)ang;
	if( ang == 0 )
		return FLT_MAX;
	if( ang == 0x4000 )
		return 0.0f;
	if( ang == 0x8000 )
		return FLT_MAX;
	if( ang == 0xc000 )
		return 0.0f;
	return 1.0f / nlTan( ang );
}

/* e to the x: 2 to a whole power times a continued fraction for what is left */
Float nlExp( Float x )
{
	Sint32 n, k;
	Float xx, p;

	n = (Sint32)( x / LN_2 + ( x >= 0.0f ? 0.5f : -0.5f ) );
	x = x - (Float)n * LN_2;
	xx = x * x;
	p = xx / 14.0f;
	for( k = 10; k >= 6; k -= 4 )
		p = xx / ( (Float)k + p );
	return nlLdexpf( ( x + p + 2.0f ) / ( p - x + 2.0f ), n );
}

Float nlFloor( Float x )
{
	Float a = fabs( x );
	Float f;
	Sint32 n;

	if( NO_FRACTION <= a )
		return x;
	n = (Sint32)a;
	f = (Float)n;
	if( x < 0.0f ){
		if( a - f != 0.0f )
			return (Float)( -1 - n );
		return x;
	}
	return f;
}

Float nlFraction( Float x )
{
	Float a = fabs( x );
	Sint32 n;

	if( NO_FRACTION <= a )
		return 0.0f;
	else {
		n = (Sint32)a;
		a = a - (Float)n;
		if( x < 0.0f )
			return -a;
		return a;
	}
}

Float nlInvertAbsSqrt( Float x )
{
	if( x == 0.0f )
		return FLT_MAX;
	return 1.0f / nlAbsSqrt( x );
}

Float nlInvertSqrt( Float x )
{
	if( x == 0.0f )
		return FLT_MAX;
	return 1.0f / sqrt( x );
}

/* x times 2 to the n, by adding n to the exponent bits */
Float nlLdexpf( Float x, int n )
{
	Float f;
	Sint32 *p;
	Sint32 sign, m;

	if( x == 0.0f )
		return 0.0f;
	p = (Sint32 *)&f;
	f = x;
	sign = 0x80000000;
	n += (Uint8)( *p >> 23 );
	if( n < 1 ){
		m = ( ( *p & 0x7fffff ) | 0x800000 ) >> ( 1 - n );
		*p &= sign;
		*p |= m;
	}else{
		if( n > 254 ){
			*p &= sign;
			*p |= 0x7f800000;
			return f;
		}
		*p &= 0x807fffff;
		*p |= n << 23;
	}
	return f;
}

/* the natural logarithm: x is brought between 1 and 2, then a series is summed until it stops changing */
Float nlLog( Float x )
{
	Sint32 n = 1;
	Float sum = 0.0f, prev, term;

	if( x < 0.0f )
		return 0.0f;
	if( x == 0.0f )
		return 0.0f;
	if( !( 2.0f > x ) ){
		do {
			x /= 2.0f;
			sum += LN_2;
		} while( !( 2.0f > x ) );
	}
	if( 1.0f > x ){
		do {
			x *= 2.0f;
			sum -= LN_2;
		} while( 1.0f > x );
	}
	x = ( x - 1.0f ) / ( x + 1.0f );
	term = x * 2.0f;
	x = x * x;
	do {
		prev = sum;
		sum += term / (Float)n;
		term *= x;
		n += 2;
	} while( sum != prev );
	return sum;
}

Float nlLog10( Float x )
{
	return nlLog( x ) / LN_10;
}

Float nlLog2( Float x )
{
	return nlLog( x ) / LN_2;
}

/* x to the y: by squaring when y is whole, else through the logarithm */
Float nlPow( Float x, Float y )
{
	Sint32 n = (Sint32)y;
	Float r;

	if( x == 0.0f ){
		if( y > 0.0f )
			return 0.0f;
		return 1.0f;
	}
	if( y == (Float)n ){
		if( n < 0 ){
			x = 1.0f / x;
			n = -n;
		}
		r = 1.0f;
		for(;;){
			if( n & 1 )
				r *= x;
			n /= 2;
			if( n == 0 )
				break;
			x *= x;
		}
		return r;
	}
	if( 0.0f > x )
		return 0.0f;
	return nlExp( y * nlLog( x ) );
}

Sint32 nlRand( void )
{
	nl_random_next = nl_random_next * 0x41c64e6d + 0x3039;
	return nl_random_next >> 16 & 0x7fff;
}

/* a random number from 0 up to 1 */
Float nlRandom( void )
{
	return (Float)nlRand() * 3.0517578125e-05f;
}

void nlRandomSeed( Uint32 seed )
{
	nl_random_next = seed;
}

/* rounds towards zero */
Float nlRoundOff( Float x )
{
	Float a = fabs( x );
	Float f;
	Sint32 n;

	if( NO_FRACTION <= a )
		return x;
	n = (Sint32)a;
	f = (Float)n;
	if( a - f != 0.0f ){
		if( x > 0.0f )
			return f;
		return (Float)-n;
	}
	return x;
}

/* rounds away from zero */
Float nlRoundUp( Float x )
{
	Float a = fabs( x );
	Sint32 n;

	if( NO_FRACTION <= a )
		return x;
	n = (Sint32)a;
	if( a - (Float)n != 0.0f ){
		if( x > 0.0f )
			return (Float)( n + 1 );
		else
			return (Float)( -1 - n );
	}
	return x;
}

Float nlSec( int ang )
{
	ang = (Uint16)ang;
	if( ang == 0x4000 )
		return FLT_MAX;
	if( ang == 0xc000 )
		return FLT_MAX;
	return 1.0f / nlCos( ang );
}

/* the sine, from the tangent of half the angle */
Float nlSin( int ang )
{
	Float h, h2, t, s;
	Sint32 n, k;

	h = (Float)(Uint16)ang * TWO_PI / 65536.0f / 2.0f;
	n = (Sint32)( h / HALF_PI + 0.5f );
	h = h - (Float)n * HALF_PI;
	h2 = h * h;
	t = 0.0f;
	for( k = 11; k >= 3; k -= 2 )
		t = h2 / ( (Float)k - t );
	h = h / ( 1.0f - t );
	s = h * 2.0f / ( h * h + 1.0f );
	if( !( n & 1 ) )
		return s;
	return -s;
}

Float nlSinh( Uint16 ang )
{
	Float x = (Float)ang * TWO_PI / 65536.0f;
	Float e;

	if( fabs( x ) > 0.01f ){
		e = nlExp( x );
		return ( e - 1.0f / e ) / 2.0f;
	}
	return ( x * x / 6.0f + 1.0f ) * x;
}

Float nlSinf( Float x )
{
	Sint32 n;
	Float t = sinf_sub( x / 2.0f, &n );
	Float s = t * 2.0f / ( t * t + 1.0f );

	if( n % 2 == 0 )
		return s;
	return -s;
}

/* the tangent of h less its nearest quarter turns, whose number goes to *n */
static Float sinf_sub( Float h, Sint32 *n )
{
	Float h2, t;
	Sint32 k, m;

	m = (Sint32)( h / HALF_PI + ( h >= 0.0f ? 0.5f : -0.5f ) );
	*n = m;
	h -= (Float)m * HALF_PI_HIGH;
	h += (Float)m * HALF_PI_LOW;
	h2 = h * h;
	t = 0.0f;
	for( k = 11; k >= 3; k -= 2 )
		t = h2 / ( (Float)k - t );
	return h / ( 1.0f - t );
}

Float nlTanf( Float x )
{
	Sint32 n;
	Float t = tanf_sub( x, &n );

	if( n % 2 == 0 )
		return t;
	if( t != 0.0f )
		return -1.0f / t;
	return FLT_MAX;
}

/* the same for nlTanf */
static Float tanf_sub( Float x, Sint32 *n )
{
	Float h2, t;
	Sint32 k, m;

	m = (Sint32)( x / HALF_PI + ( x >= 0.0f ? 0.5f : -0.5f ) );
	*n = m;
	x -= (Float)m * HALF_PI_HIGH;
	x += (Float)m * HALF_PI_LOW;
	h2 = x * x;
	t = 0.0f;
	for( k = 11; k >= 3; k -= 2 )
		t = h2 / ( (Float)k - t );
	return x / ( 1.0f - t );
}

Float nlTan( int ang )
{
	Float x, h2, t;
	Sint32 n, k;

	ang = (Uint16)ang;
	if( ( ang & 0x7fff ) == 0x4000 )
		return FLT_MAX;
	x = (Float)(Uint16)ang * TWO_PI / 65536.0f;
	n = (Sint32)( x / HALF_PI + 0.5f );
	x = x - (Float)n * HALF_PI;
	h2 = x * x;
	t = 0.0f;
	for( k = 13; k >= 3; k -= 2 )
		t = h2 / ( (Float)k - t );
	t = x / ( 1.0f - t );
	if( !( n & 1 ) )
		return t;
	if( t != 0.0f )
		return -1.0f / t;
	return FLT_MAX;
}

#ifdef HOST_PORT
/* Angle of (x, y) in 256ths of a turn (0..255), matching nlmatha.src's table and octants. */
Sint32 nlAtan256( Float y, Float x )
{
	static const Uint8 nlAtan256tbl[129] = {
		0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 5,
		5, 5, 6, 6, 6, 7, 7, 7, 8, 8, 8, 8, 9, 9, 9, 10,
		10, 10, 11, 11, 11, 11, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14,
		15, 15, 15, 15, 16, 16, 16, 17, 17, 17, 17, 18, 18, 18, 18, 19,
		19, 19, 19, 20, 20, 20, 20, 21, 21, 21, 21, 22, 22, 22, 22, 23,
		23, 23, 23, 23, 24, 24, 24, 24, 25, 25, 25, 25, 25, 26, 26, 26,
		26, 26, 27, 27, 27, 27, 27, 28, 28, 28, 28, 28, 29, 29, 29, 29,
		29, 29, 30, 30, 30, 30, 30, 31, 31, 31, 31, 31, 31, 32, 32, 32,
		32
	};
	static const Uint16 octant[8] = {
		0x0000, 0x0101, 0x0081, 0x0080, 0x0201, 0x0100, 0x0180, 0x0181
	};
	Float ax, ay, num, den;
	Sint32 oct, idx, ang, base;

	oct = 0;
	if( y < 0.0f ){
		oct |= 4;
		ay = -y;
	} else {
		ay = y;
	}
	if( x < 0.0f ){
		oct |= 1;
		ax = -x;
	} else {
		ax = x;
	}
	if( ay > ax ){
		oct |= 2;
		num = ax;
		den = ay;
	} else {
		num = ay;
		den = ax;
	}
	if( den == 0.0f )
		return 0;
	idx = (Sint32)( 128.0f * ( num / den ) + 0.5f );
	if( idx > 128 )
		idx = 128;
	ang = nlAtan256tbl[idx];
	base = octant[oct] >> 1;
	if( octant[oct] & 1 )
		ang = -ang;
	return ( base + ang ) & 0xff;
}
#endif

Float nlTanh( Uint16 ang )
{
	Float x = (Float)ang * TWO_PI / 65536.0f;

	if( x > 0.001f )
		return 2.0f / ( nlExp( -( x * 2.0f ) ) + 1.0f ) - 1.0f;
	if( 0.001f > x )
		return -( 2.0f / ( nlExp( x * 2.0f ) + 1.0f ) ) + 1.0f;
	return ( 1.0f - x * x / 3.0f ) * x;
}
