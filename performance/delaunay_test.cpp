///////////////////////////////////////////////////////////////////////////////
//  Copyright 2012 John Maddock. 
//  Copyright 2012 Phil Endecott
//  Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <boost/multiprecision/cpp_int.hpp>
#include "arithmetic_backend.hpp"
#include <boost/chrono.hpp>

#include <fstream>
#include <iomanip>

template <class Clock>
struct stopwatch
{
   typedef typename Clock::duration duration;
   stopwatch()
   {
      m_start = Clock::now();
   }
   duration elapsed()
   {
      return Clock::now() - m_start;
   }
   void reset()
   {
      m_start = Clock::now();
   }

private:
   typename Clock::time_point m_start;
};

// Custom 128-bit maths used for exact calculation of the Delaunay test.
// Only the few operators actually needed here are implemented.

struct int128_t {
  int64_t high;
  uint64_t low;

  int128_t() {}
  int128_t(int32_t i): high(i>>31), low(static_cast<int64_t>(i)) {}
  int128_t(uint32_t i): high(0), low(i) {}
  int128_t(int64_t i): high(i>>63), low(i) {}
  int128_t(uint64_t i): high(0), low(i) {}
};

inline int128_t operator<<(int128_t val, int amt)
{
  int128_t r;
  r.low = val.low << amt;
  r.high = val.low >> (64-amt);
  r.high |= val.high << amt;
  return r;
}

inline int128_t& operator+=(int128_t& l, int128_t r)
{
  l.low += r.low;
  bool carry = l.low < r.low;
  l.high += r.high;
  if (carry) ++l.high;
  return l;
}

inline int128_t operator-(int128_t val)
{
  val.low = ~val.low;
  val.high = ~val.high;
  val.low += 1;
  if (val.low == 0) val.high += 1;
  return val;
}

inline int128_t operator+(int128_t l, int128_t r)
{
  l += r;
  return l;
}

inline bool operator<(int128_t l, int128_t r)
{
  if (l.high != r.high) return l.high < r.high;
  return l.low < r.low;
}


inline int128_t mult_64x64_to_128(int64_t a, int64_t b)
{
  // Make life simple by dealing only with positive numbers:
  bool neg = false;
  if (a<0) { neg = !neg; a = -a; }
  if (b<0) { neg = !neg; b = -b; }

  // Divide input into 32-bit halves:
  uint32_t ah = a >> 32;
  uint32_t al = a & 0xffffffff;
  uint32_t bh = b >> 32;
  uint32_t bl = b & 0xffffffff;

  // Long multiplication, with 64-bit temporaries:

  //            ah al
  //          * bh bl
  // ----------------
  //            al*bl   (t1)
  // +       ah*bl      (t2)
  // +       al*bh      (t3)
  // +    ah*bh         (t4)
  // ----------------

  uint64_t t1 = static_cast<uint64_t>(al)*bl;
  uint64_t t2 = static_cast<uint64_t>(ah)*bl;
  uint64_t t3 = static_cast<uint64_t>(al)*bh;
  uint64_t t4 = static_cast<uint64_t>(ah)*bh;

  int128_t r(t1);
  r.high = t4;
  r += int128_t(t2) << 32;
  r += int128_t(t3) << 32;

  if (neg) r = -r;

  return r;
}

template <class R, class T>
__forceinline void mul_2n(R& r, const T& a, const T& b)
{
   r = R(a) * b;
}

__forceinline void mul_2n(int128_t& r, const boost::int64_t& a, const boost::int64_t& b)
{
   r = mult_64x64_to_128(a, b);
}

template <class Traits>
inline bool delaunay_test(int32_t ax, int32_t ay, int32_t bx, int32_t by,
   int32_t cx, int32_t cy, int32_t dx, int32_t dy)
{
   // Test whether the quadrilateral ABCD's diagonal AC should be flipped to BD.
   // This is the Cline & Renka method.
   // Flip if the sum of the angles ABC and CDA is greater than 180 degrees.
   // Equivalently, flip if sin(ABC + CDA) < 0.
   // Trig identity: cos(ABC) * sin(CDA) + sin(ABC) * cos(CDA) < 0
   // We can use scalar and vector products to find sin and cos, and simplify 
   // to the following code.
   // Numerical robustness is important.  This code addresses it by performing 
   // exact calculations with large integer types.

   typedef typename Traits::i64_t i64;
   typedef typename Traits::i128_t i128;

   i64 ax64 = ax, ay64 = ay, bx64 = bx, by64 = by,
      cx64 = cx, cy64 = cy, dx64 = dx, dy64 = dy;

   i64 cos_abc, t;
   mul_2n(cos_abc, (ax-bx), (cx-bx));
   mul_2n(t, (ay-by), (cy-by));
   cos_abc += t;

   i64 cos_cda;
   mul_2n(cos_cda, (cx-dx), (ax-dx));
   mul_2n(t, (cy-dy), (ay-dy));
   cos_cda += t;

   if (cos_abc >= 0 && cos_cda >= 0) return false;
   if (cos_abc < 0 && cos_cda < 0) return true;

   i64 sin_abc;
   mul_2n(sin_abc, (ax-bx), (cy-by));
   mul_2n(t, (cx-bx), (ay-by));
   sin_abc -= t;

   i64 sin_cda; 
   mul_2n(sin_cda, (cx-dx), (ay-dy));
   mul_2n(t, (ax-dx), (cy-dy));
   sin_cda -= t;

   i128 sin_sum, t128;
   mul_2n(sin_sum, sin_abc, cos_cda);
   mul_2n(t128, cos_abc, sin_cda);
   sin_sum += t128;

   return sin_sum < 0;
}


struct dt_dat {
   int32_t ax, ay, bx, by, cx, cy, dx, dy;
};

typedef std::vector<dt_dat> data_t;
data_t data;

static void load_data()
{
   std::ifstream is("delaunay_data.txt");

   while (is.good()) 
   {
      dt_dat d;
      is >> d.ax >> d.ay >> d.bx >> d.by >> d.cx >> d.cy >> d.dx >> d.dy;
      if (is.good()) 
      {
         data.push_back(d);
         d.ax >>= 10;
         d.ay >>= 10;
         d.bx >>= 10;
         d.by >>= 10;
         d.cx >>= 10;
         d.cy >>= 10;
         d.dx >>= 10;
         d.dy >>= 10;
         data.push_back(d);
      }
   }
}

template <class Traits>
void do_calc(const char* name)
{
   std::cout << "Running calculations for: " << name << std::endl;

   stopwatch<boost::chrono::high_resolution_clock> w;

   boost::uint64_t flips = 0;
   boost::uint64_t calcs = 0;

   for(int j = 0; j < 1000; ++j) 
   {
      for(data_t::const_iterator i = data.begin(); i != data.end(); ++i) 
      {
         const dt_dat& d = *i;
         bool flip = delaunay_test<Traits>(d.ax,d.ay, d.bx,d.by, d.cx,d.cy, d.dx,d.dy);
         if (flip) ++flips;
         ++calcs;
      }
   }
   double t = boost::chrono::duration_cast<boost::chrono::duration<double> >(w.elapsed()).count();

   std::cout << "Number of calculations = " << calcs << std::endl;
   std::cout << "Number of flips = " << flips << std::endl;
   std::cout << "Total execution time = " << t << std::endl;
   std::cout << "Time per calculation = " << t / calcs << std::endl << std::endl;
}

template <class I64, class I128>
struct test_traits
{
   typedef I64 i64_t;
   typedef I128 i128_t;
};


int main()
{
   using namespace boost::multiprecision;
   std::cout << "loading data...\n";
   load_data();

   std::cout << "calculating...\n";

   do_calc<test_traits<boost::int64_t, boost::int64_t> >("int64_t, int64_t");
   do_calc<test_traits<mp_number<arithmetic_backend<boost::int64_t> >, mp_number<arithmetic_backend<boost::int64_t> > > >("arithmetic_backend<int64_t>");
   do_calc<test_traits<boost::int64_t, int128_t> >("int64_t, int128_t");
   do_calc<test_traits<boost::int64_t, mp_int128_t> >("int64_t, mp_int128_t");
   do_calc<test_traits<boost::int64_t, mp_number<cpp_int_backend<128, true, void>, true> > >("int64_t, mp_int128_t (ET)");
   do_calc<test_traits<boost::int64_t, cpp_int> >("int64_t, cpp_int");
   do_calc<test_traits<boost::int64_t, mp_number<cpp_int_backend<>, false> > >("int64_t, cpp_int (no ET's)");
   do_calc<test_traits<boost::int64_t, mp_number<cpp_int_backend<128> > > >("int64_t, cpp_int(128-bit cache)");
   do_calc<test_traits<boost::int64_t, mp_number<cpp_int_backend<128>, false> > >("int64_t, cpp_int (128-bit Cache no ET's)");

   return 0;
}
