/*
** time.c - Time class
**
** See Copyright Notice in mruby.h
*/

#include <mruby.h>
#include <mruby/class.h>
#include <mruby/data.h>
#include <mruby/numeric.h>
#include <mruby/time.h>
#include <mruby/string.h>
#include <mruby/internal.h>
#include <mruby/presym.h>

#ifdef MRB_NO_STDIO
#include <string.h>
#endif


#include <stdlib.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#define NDIV(x,y) (-(-((x)+1)/(y))-1)
#define TO_S_FMT "%Y-%m-%d %H:%M:%S "

#if defined(_MSC_VER) && _MSC_VER < 1800
double round(double x) {
  return floor(x + 0.5);
}
#endif

#ifndef MRB_NO_FLOAT
# if !defined(__MINGW64__) && defined(_WIN32)
#  define llround(x) round(x)
# endif
#endif

#if defined(__MINGW64__) || defined(__MINGW32__)
# include <sys/time.h>
#endif

/** Time class configuration */

/* gettimeofday(2) */
/* C99 does not have gettimeofday that is required to retrieve microseconds */
/* uncomment following macro on platforms without gettimeofday(2) */
/* #define NO_GETTIMEOFDAY */

/* gmtime(3) */
/* C99 does not have reentrant gmtime_r() so it might cause troubles under */
/* multi-threading environment.  undef following macro on platforms that */
/* does not have gmtime_r() and localtime_r(). */
/* #define NO_GMTIME_R */

#ifdef _WIN32
#ifdef _MSC_VER
/* Win32 platform do not provide gmtime_r/localtime_r; emulate them using gmtime_s/localtime_s */
#define gmtime_r(tp, tm)    ((gmtime_s((tm), (tp)) == 0) ? (tm) : NULL)
#define localtime_r(tp, tm)    ((localtime_s((tm), (tp)) == 0) ? (tm) : NULL)
#else
#define NO_GMTIME_R
#endif
#endif
#ifdef __STRICT_ANSI__
/* Strict ANSI (e.g. -std=c99) do not provide gmtime_r/localtime_r */
#define NO_GMTIME_R
#endif

/* asctime(3) */
/* mruby usually use its own implementation of struct tm to string conversion */
/* except when MRB_NO_STDIO is set. In that case, it uses asctime() or asctime_r(). */
/* By default mruby tries to use asctime_r() which is reentrant. */
/* Undef following macro on platforms that does not have asctime_r(). */
/* #define NO_ASCTIME_R */

/* timegm(3) */
/* mktime() creates tm structure for localtime; timegm() is for UTC time */
/* define following macro to use probably faster timegm() on the platform */
/* #define USE_SYSTEM_TIMEGM */

/** end of Time class configuration */

#if (defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0) && defined(CLOCK_REALTIME)
# define USE_CLOCK_GETTIME
#endif

#if !defined(NO_GETTIMEOFDAY)
# if defined(_WIN32) && !defined(USE_CLOCK_GETTIME)
#  define WIN32_LEAN_AND_MEAN  /* don't include winsock.h */
#  include <windows.h>
#  define gettimeofday my_gettimeofday

#  ifdef _MSC_VER
#    define UI64(x) x##ui64
#  else
#    define UI64(x) x##ull
#  endif

typedef long suseconds_t;

# if (!defined __MINGW64__) && (!defined __MINGW32__)
struct timeval {
  time_t tv_sec;
  suseconds_t tv_usec;
};
# endif

static int
gettimeofday(struct timeval *tv, void *tz)
{
  if (tz) {
    mrb_assert(0);  /* timezone is not supported */
  }
  if (tv) {
    union {
      FILETIME ft;
      unsigned __int64 u64;
    } t;
    GetSystemTimeAsFileTime(&t.ft);   /* 100 ns intervals since Windows epoch */
    t.u64 -= UI64(116444736000000000);  /* Unix epoch bias */
    t.u64 /= 10;                      /* to microseconds */
    tv->tv_sec = (time_t)(t.u64 / (1000 * 1000));
    tv->tv_usec = t.u64 % (1000 * 1000);
  }
  return 0;
}
# else
#  include <sys/time.h>
# endif
#endif
#ifdef NO_GMTIME_R
#define gmtime_r(t,r) gmtime(t)
#define localtime_r(t,r) localtime(t)
#endif

#ifndef USE_SYSTEM_TIMEGM
#define timegm my_timgm

static unsigned int
is_leapyear(unsigned int y)
{
  return (y % 4) == 0 && ((y % 100) != 0 || (y % 400) == 0);
}

static time_t
timegm(struct tm *tm)
{
  static const unsigned int ndays[2][12] = {
    {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
    {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}
  };
  time_t r = 0;
  int i;
  unsigned int *nday = (unsigned int*) ndays[is_leapyear(tm->tm_year+1900)];

  static const int epoch_year = 70;
  if (tm->tm_year >= epoch_year) {
    for (i = epoch_year; i < tm->tm_year; ++i)
      r += is_leapyear(i+1900) ? 366*24*60*60 : 365*24*60*60;
  }
  else {
    for (i = tm->tm_year; i < epoch_year; ++i)
      r -= is_leapyear(i+1900) ? 366*24*60*60 : 365*24*60*60;
  }
  for (i = 0; i < tm->tm_mon; ++i)
    r += nday[i] * 24 * 60 * 60;
  r += (tm->tm_mday - 1) * 24 * 60 * 60;
  r += tm->tm_hour * 60 * 60;
  r += tm->tm_min * 60;
  r += tm->tm_sec;
  return r;
}
#endif

/* Since we are limited to using ISO C99, this implementation is based
* on time_t. That means the resolution of time is only precise to the
* second level. Also, there are only 2 timezones, namely UTC and LOCAL.
*/

#ifndef MRB_NO_STDIO
static const char mon_names[12][4] = {
  "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
};

static const char wday_names[7][4] = {
  "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat",
};
#endif

struct mrb_time {
  time_t              sec;
  time_t              usec;
  enum mrb_timezone   timezone;
  struct tm           datetime;
};

static const struct mrb_data_type time_type = { "Time", mrb_free };

#define MRB_TIME_T_UINT (~(time_t)0 > 0)
#define MRB_TIME_MIN (                                                      \
  MRB_TIME_T_UINT ? 0 :                                                     \
                    (sizeof(time_t) <= 4 ? INT32_MIN : INT64_MIN)           \
)
#define MRB_TIME_MAX (time_t)(                                              \
  MRB_TIME_T_UINT ? (sizeof(time_t) <= 4 ? UINT32_MAX : UINT64_MAX) :       \
                    (sizeof(time_t) <= 4 ? INT32_MAX : INT64_MAX)           \
)

/* return true if time_t is fit in mrb_int */
static mrb_bool
fixable_time_t_p(time_t v)
{
  if (MRB_INT_MIN <= MRB_TIME_MIN && MRB_TIME_MAX <= MRB_INT_MAX) return TRUE;
  if (v > (time_t)MRB_INT_MAX) return FALSE;
  if (MRB_TIME_T_UINT) return TRUE;
  if (MRB_INT_MIN > (mrb_int)v) return FALSE;
  return TRUE;
}

static time_t
mrb_to_time_t(mrb_state *mrb, mrb_value obj, time_t *usec)
{
  time_t t;

  switch (mrb_type(obj)) {
#ifndef MRB_NO_FLOAT
    case MRB_TT_FLOAT:
      {
        mrb_float f = mrb_float(obj);

        mrb_check_num_exact(mrb, f);
        if (f >= ((mrb_float)MRB_TIME_MAX-1.0) || f < ((mrb_float)MRB_TIME_MIN+1.0)) {
          goto out_of_range;
        }

        if (usec) {
          double tt = floor(f);
          if (!isfinite(tt)) goto out_of_range;
          t = (time_t)tt;
          *usec = (time_t)trunc((f - tt) * 1.0e+6);
        }
        else {
          double tt = round(f);
          if (!isfinite(tt)) goto out_of_range;
          t = (time_t)tt;
        }
      }
      break;
#endif /* MRB_NO_FLOAT */

#ifdef MRB_USE_BIGINT
    case MRB_TT_BIGINT:
      {
        if (sizeof(time_t) > sizeof(mrb_int)) {
          if (MRB_TIME_T_UINT) {
            t = (time_t)mrb_bint_as_uint64(mrb, obj);
          }
          else {
            t = (time_t)mrb_bint_as_int64(mrb, obj);
          }
          if (usec) { *usec = 0; }
          break;
        }
        else {
          mrb_int i = mrb_bint_as_int(mrb, obj);
          obj = mrb_int_value(mrb, i);
        }
      }
      /* fall through */
#endif  /* MRB_USE_BIGINT */

    case MRB_TT_INTEGER:
      {
        mrb_int i = mrb_integer(obj);

        if ((MRB_INT_MAX > MRB_TIME_MAX && i > 0 && (time_t)i > MRB_TIME_MAX) ||
            (0 > MRB_TIME_MIN && MRB_TIME_MIN > MRB_INT_MIN && MRB_TIME_MIN > i)) {
          goto out_of_range;
        }

        t = (time_t)i;
        if (usec) { *usec = 0; }
      }
      break;

    default:
      mrb_raisef(mrb, E_TYPE_ERROR, "cannot convert %Y to time", obj);
      return 0;
  }

  return t;

out_of_range:
  mrb_raisef(mrb, E_ARGUMENT_ERROR, "%v out of Time range", obj);

  /* not reached */
  return 0;
}

static mrb_value
time_value_from_time_t(mrb_state *mrb, time_t t)
{
  if (!fixable_time_t_p(t)) {
#if defined(MRB_USE_BIGINT)
    if (MRB_TIME_T_UINT) {
      return mrb_bint_new_uint64(mrb, (uint64_t)t);
    }
    else {
      return mrb_bint_new_int64(mrb, (int64_t)t);
    }
#elif !defined(MRB_NO_FLOAT)
    return mrb_float_value(mrb, (mrb_float)t);
#else
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "Time too big");
#endif
  }
  return mrb_int_value(mrb, (mrb_int)t);
}

/** Updates the datetime of a mrb_time based on it's timezone and
    seconds setting. Returns self on success, NULL of failure.
    if `dealloc` is set `true`, it frees `self` on error. */
static struct mrb_time*
time_update_datetime(mrb_state *mrb, struct mrb_time *self, int dealloc)
{
  struct tm *aid;
  time_t t = self->sec;

  if (self->timezone == MRB_TIMEZONE_UTC) {
    aid = gmtime_r(&t, &self->datetime);
  }
  else {
    aid = localtime_r(&t, &self->datetime);
  }
  if (!aid) {
    if (dealloc) mrb_free(mrb, self);
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "%v out of Time range", time_value_from_time_t(mrb, t));
    /* not reached */
    return NULL;
  }
#ifdef NO_GMTIME_R
  self->datetime = *aid; /* copy data */
#endif

  return self;
}

static mrb_value
time_wrap(mrb_state *mrb, struct RClass *tc, struct mrb_time *tm)
{
  return mrb_obj_value(Data_Wrap_Struct(mrb, tc, &time_type, tm));
}

/* Allocates a mrb_time object and initializes it. */
static struct mrb_time*
time_alloc_time(mrb_state *mrb, time_t sec, time_t usec, enum mrb_timezone timezone)
{
  struct mrb_time *tm;

  tm = (struct mrb_time*)mrb_malloc(mrb, sizeof(struct mrb_time));
  tm->sec  = sec;
  tm->usec = usec;
  if (!MRB_TIME_T_UINT && tm->usec < 0) {
    long sec2 = (long)NDIV(tm->usec,1000000); /* negative div */
    tm->usec -= sec2 * 1000000;
    tm->sec += sec2;
  }
  else if (tm->usec >= 1000000) {
    long sec2 = (long)(tm->usec / 1000000);
    tm->usec -= sec2 * 1000000;
    tm->sec += sec2;
  }
  tm->timezone = timezone;
  time_update_datetime(mrb, tm, TRUE);

  return tm;
}

static struct mrb_time*
time_alloc(mrb_state *mrb, mrb_value sec, mrb_value usec, enum mrb_timezone timezone)
{
  time_t tsec, tusec;

  tsec = mrb_to_time_t(mrb, sec, &tusec);
  tusec += mrb_to_time_t(mrb, usec, NULL);

  return time_alloc_time(mrb, tsec, tusec, timezone);
}

static mrb_value
time_make_time(mrb_state *mrb, struct RClass *c, time_t sec, time_t usec, enum mrb_timezone timezone)
{
  return time_wrap(mrb, c, time_alloc_time(mrb, sec, usec, timezone));
}

static mrb_value
time_make(mrb_state *mrb, struct RClass *c, mrb_value sec, mrb_value usec, enum mrb_timezone timezone)
{
  return time_wrap(mrb, c, time_alloc(mrb, sec, usec, timezone));
}

static struct mrb_time*
current_mrb_time(mrb_state *mrb)
{
  struct mrb_time tmzero = {0};
  struct mrb_time *tm;
  time_t sec, usec;

#if defined(TIME_UTC) && !defined(__ANDROID__)
  {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    sec = ts.tv_sec;
    usec = ts.tv_nsec / 1000;
  }
#elif defined(USE_CLOCK_GETTIME)
  {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    sec = ts.tv_sec;
    usec = ts.tv_nsec / 1000;
  }
#elif defined(NO_GETTIMEOFDAY)
  {
    static time_t last_sec = 0, last_usec = 0;

    sec = time(NULL);
    if (sec != last_sec) {
      last_sec = sec;
      last_usec = 0;
    }
    else {
      /* add 1 usec to differentiate two times */
      last_usec += 1;
    }
    usec = last_usec;
  }
#else
  {
    struct timeval tv;

    gettimeofday(&tv, NULL);
    sec = tv.tv_sec;
    usec = tv.tv_usec;
  }
#endif
  tm = (struct mrb_time*)mrb_malloc(mrb, sizeof(*tm));
  *tm = tmzero;
  tm->sec = sec; tm->usec = usec;
  tm->timezone = MRB_TIMEZONE_LOCAL;
  time_update_datetime(mrb, tm, TRUE);

  return tm;
}

/* Allocates a new Time object with given millis value. */
static mrb_value
time_now(mrb_state *mrb, mrb_value self)
{
  return time_wrap(mrb, mrb_class_ptr(self), current_mrb_time(mrb));
}

MRB_API mrb_value
time_at(mrb_state *mrb, time_t sec, time_t usec, enum mrb_timezone zone)
{
  return time_make_time(mrb, mrb_class_get_id(mrb, MRB_SYM(Time)), sec, usec, zone);
}

/* 15.2.19.6.1 */
/* Creates an instance of time at the given time in seconds, etc. */
static mrb_value
time_at_m(mrb_state *mrb, mrb_value self)
{
  mrb_value sec;
  mrb_value usec = mrb_fixnum_value(0);

  mrb_get_args(mrb, "o|o", &sec, &usec);

  return time_make(mrb, mrb_class_ptr(self), sec, usec, MRB_TIMEZONE_LOCAL);
}

static struct mrb_time*
time_mktime(mrb_state *mrb, mrb_int ayear, mrb_int amonth, mrb_int aday,
  mrb_int ahour, mrb_int amin, mrb_int asec, mrb_int ausec,
  enum mrb_timezone timezone)
{
  time_t nowsecs;
  struct tm nowtime = { 0 };

#if MRB_INT_MAX > INT_MAX
#define OUTINT(x) (((MRB_TIME_T_UINT ? 0 : INT_MIN) > (x)) || (x) > INT_MAX)
#else
#define OUTINT(x) 0
#endif

  ayear -= 1900;
  if (OUTINT(ayear) ||
      amonth  < 1 || amonth  > 12 ||
      aday    < 1 || aday    > 31 ||
      ahour   < 0 || ahour   > 24 ||
      (ahour == 24 && (amin > 0 || asec > 0)) ||
      amin    < 0 || amin    > 59 ||
      asec    < 0 || asec    > 60)
    mrb_raise(mrb, E_ARGUMENT_ERROR, "argument out of range");

  nowtime.tm_year  = (int)ayear;
  nowtime.tm_mon   = (int)(amonth - 1);
  nowtime.tm_mday  = (int)aday;
  nowtime.tm_hour  = (int)ahour;
  nowtime.tm_min   = (int)amin;
  nowtime.tm_sec   = (int)asec;
  nowtime.tm_isdst = -1;

  time_t (*mk)(struct tm*);
  if (timezone == MRB_TIMEZONE_UTC) {
    mk = timegm;
  }
  else {
    mk = mktime;
  }
  nowsecs = (*mk)(&nowtime);
  if (nowsecs == (time_t)-1) {
    nowtime.tm_sec += 1;        /* maybe Epoch-1 sec */
    nowsecs = (*mk)(&nowtime);
    if (nowsecs != 0) {         /* check if Epoch */
      mrb_raise(mrb, E_ARGUMENT_ERROR, "Not a valid time");
    }
    nowsecs = (time_t)-1;       /* valid Epoch-1 */
  }

  return time_alloc_time(mrb, nowsecs, ausec, timezone);
}

/* 15.2.19.6.2 */
/* Creates an instance of time at the given time in UTC. */
static mrb_value
time_gm(mrb_state *mrb, mrb_value self)
{
  mrb_int ayear = 0, amonth = 1, aday = 1, ahour = 0, amin = 0, asec = 0, ausec = 0;

  mrb_get_args(mrb, "i|iiiiii",
                &ayear, &amonth, &aday, &ahour, &amin, &asec, &ausec);
  return time_wrap(mrb, mrb_class_ptr(self),
          time_mktime(mrb, ayear, amonth, aday, ahour, amin, asec, ausec, MRB_TIMEZONE_UTC));
}


/* 15.2.19.6.3 */
/* Creates an instance of time at the given time in local time zone. */
static mrb_value
time_local(mrb_state *mrb, mrb_value self)
{
  mrb_int ayear = 0, amonth = 1, aday = 1, ahour = 0, amin = 0, asec = 0, ausec = 0;

  mrb_get_args(mrb, "i|iiiiii",
                &ayear, &amonth, &aday, &ahour, &amin, &asec, &ausec);
  return time_wrap(mrb, mrb_class_ptr(self),
          time_mktime(mrb, ayear, amonth, aday, ahour, amin, asec, ausec, MRB_TIMEZONE_LOCAL));
}

static struct mrb_time*
time_get_ptr(mrb_state *mrb, mrb_value time)
{
  struct mrb_time *tm;

  tm = DATA_GET_PTR(mrb, time, &time_type, struct mrb_time);
  if (!tm) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "uninitialized time");
  }
  return tm;
}

static mrb_value
time_eq(mrb_state *mrb, mrb_value self)
{
  mrb_value other = mrb_get_arg1(mrb);
  struct mrb_time *tm1, *tm2;
  mrb_bool eq_p;

  tm1 = DATA_GET_PTR(mrb, self, &time_type, struct mrb_time);
  tm2 = DATA_CHECK_GET_PTR(mrb, other, &time_type, struct mrb_time);
  eq_p = tm1 && tm2 && tm1->sec == tm2->sec && tm1->usec == tm2->usec;

  return mrb_bool_value(eq_p);
}

static mrb_value
time_cmp(mrb_state *mrb, mrb_value self)
{
  mrb_value other = mrb_get_arg1(mrb);
  struct mrb_time *tm1, *tm2;

  tm1 = DATA_GET_PTR(mrb, self, &time_type, struct mrb_time);
  tm2 = DATA_CHECK_GET_PTR(mrb, other, &time_type, struct mrb_time);
  if (!tm1 || !tm2) return mrb_nil_value();
  if (tm1->sec > tm2->sec) {
    return mrb_fixnum_value(1);
  }
  else if (tm1->sec < tm2->sec) {
    return mrb_fixnum_value(-1);
  }
  /* tm1->sec == tm2->sec */
  if (tm1->usec > tm2->usec) {
    return mrb_fixnum_value(1);
  }
  else if (tm1->usec < tm2->usec) {
    return mrb_fixnum_value(-1);
  }
  return mrb_fixnum_value(0);
}

static mrb_noreturn void
int_overflow(mrb_state *mrb, const char *reason)
{
  mrb_raisef(mrb, E_RANGE_ERROR, "time_t overflow in Time %s", reason);
}

static mrb_value
time_plus(mrb_state *mrb, mrb_value self)
{
  mrb_value o = mrb_get_arg1(mrb);
  struct mrb_time *tm;
  time_t sec, usec;

  tm = time_get_ptr(mrb, self);
  sec = mrb_to_time_t(mrb, o, &usec);
#ifdef MRB_HAVE_TYPE_GENERIC_CHECKED_ARITHMETIC_BUILTINS
  if (__builtin_add_overflow(tm->sec, sec, &sec)) {
    int_overflow(mrb, "addition");
  }
#else
  if (sec >= 0) {
    if (tm->sec > MRB_TIME_MAX - sec) {
      int_overflow(mrb, "addition");
    }
  }
  else {
    if (tm->sec < MRB_TIME_MIN - sec) {
      int_overflow(mrb, "addition");
    }
  }
  sec = tm->sec + sec;
#endif
  return time_make_time(mrb, mrb_obj_class(mrb, self), sec, tm->usec+usec, tm->timezone);
}

static mrb_value
time_minus(mrb_state *mrb, mrb_value self)
{
  mrb_value other = mrb_get_arg1(mrb);
  struct mrb_time *tm, *tm2;

  tm = time_get_ptr(mrb, self);
  tm2 = DATA_CHECK_GET_PTR(mrb, other, &time_type, struct mrb_time);
  if (tm2) {
#ifndef MRB_NO_FLOAT
    mrb_float f;
    f = (mrb_float)(tm->sec - tm2->sec)
      + (mrb_float)(tm->usec - tm2->usec) / 1.0e6;
    return mrb_float_value(mrb, f);
#else
    mrb_int f;
    f = tm->sec - tm2->sec;
    if (tm->usec < tm2->usec) f--;
    return mrb_int_value(mrb, f);
#endif
  }
  else {
    time_t sec, usec;
    sec = mrb_to_time_t(mrb, other, &usec);
#ifdef MRB_HAVE_TYPE_GENERIC_CHECKED_ARITHMETIC_BUILTINS
    if (__builtin_sub_overflow(tm->sec, sec, &sec)) {
      int_overflow(mrb, "subtraction");
    }
#else
    if (sec >= 0) {
      if (tm->sec < MRB_TIME_MIN + sec) {
        int_overflow(mrb, "subtraction");
      }
    }
    else {
      if (tm->sec > MRB_TIME_MAX + sec) {
        int_overflow(mrb, "subtraction");
      }
    }
    sec = tm->sec - sec;
#endif
    return time_make_time(mrb, mrb_obj_class(mrb, self), sec, tm->usec-usec, tm->timezone);
  }
}

/* 15.2.19.7.30 */
/* Returns week day number of time. */
static mrb_value
time_wday(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm;

  tm = time_get_ptr(mrb, self);
  return mrb_fixnum_value(tm->datetime.tm_wday);
}

/* 15.2.19.7.31 */
/* Returns year day number of time. */
static mrb_value
time_yday(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm;

  tm = time_get_ptr(mrb, self);
  return mrb_fixnum_value(tm->datetime.tm_yday + 1);
}

/* 15.2.19.7.32 */
/* Returns year of time. */
static mrb_value
time_year(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm;

  tm = time_get_ptr(mrb, self);
  return mrb_fixnum_value(tm->datetime.tm_year + 1900);
}

static size_t
time_zonename(mrb_state *mrb, struct mrb_time *tm, char *buf, size_t len)
{
#if defined(_MSC_VER) && _MSC_VER < 1900 || defined(__MINGW64__) || defined(__MINGW32__)
  struct tm datetime = {0};
  time_t utc_sec = timegm(&tm->datetime);
  int offset = abs((int)(utc_sec - tm->sec) / 60);
  datetime.tm_year = 100;
  datetime.tm_hour = offset / 60;
  datetime.tm_min = offset % 60;
  buf[0] = utc_sec < tm->sec ? '-' : '+';
  return strftime(buf+1, len-1, "%H%M", &datetime) + 1;
#else
  return strftime(buf, len, "%z", &tm->datetime);
#endif
}

/* 15.2.19.7.33 */
/* Returns name of time's timezone. */
static mrb_value
time_zone(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm = time_get_ptr(mrb, self);
  if (tm->timezone == MRB_TIMEZONE_UTC) {
    return mrb_str_new_lit(mrb, "UTC");
  }
  char buf[64];
  size_t len = time_zonename(mrb, tm, buf, sizeof(buf));
  return mrb_str_new(mrb, buf, len);
}

/* 15.2.19.7.4 */
/* Returns a string that describes the time. */
static mrb_value
time_asctime(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm = time_get_ptr(mrb, self);
  struct tm *d = &tm->datetime;
  int len;

#if defined(MRB_NO_STDIO)
# ifdef NO_ASCTIME_R
  char *buf = asctime(d);
# else
  char buf[32], *s;
  s = asctime_r(d, buf);
# endif
  len = strlen(buf)-1;       /* truncate the last newline */
#else
  char buf[32];

  len = snprintf(buf, sizeof(buf), "%s %s %2d %02d:%02d:%02d %.4d",
    wday_names[d->tm_wday], mon_names[d->tm_mon], d->tm_mday,
    d->tm_hour, d->tm_min, d->tm_sec,
    d->tm_year + 1900);
#endif
  return mrb_str_new(mrb, buf, len);
}

/* 15.2.19.7.6 */
/* Returns the day in the month of the time. */
static mrb_value
time_day(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm;

  tm = time_get_ptr(mrb, self);
  return mrb_fixnum_value(tm->datetime.tm_mday);
}


/* 15.2.19.7.7 */
/* Returns true if daylight saving was applied for this time. */
static mrb_value
time_dst_p(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm;

  tm = time_get_ptr(mrb, self);
  return mrb_bool_value(tm->datetime.tm_isdst);
}

/* 15.2.19.7.8 */
/* 15.2.19.7.10 */
/* Returns the Time object of the UTC(GMT) timezone. */
static mrb_value
time_getutc(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm, *tm2;

  tm = time_get_ptr(mrb, self);
  tm2 = (struct mrb_time*)mrb_malloc(mrb, sizeof(*tm));
  *tm2 = *tm;
  tm2->timezone = MRB_TIMEZONE_UTC;
  time_update_datetime(mrb, tm2, TRUE);
  return time_wrap(mrb, mrb_obj_class(mrb, self), tm2);
}

/* 15.2.19.7.9 */
/* Returns the Time object of the LOCAL timezone. */
static mrb_value
time_getlocal(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm, *tm2;

  tm = time_get_ptr(mrb, self);
  tm2 = (struct mrb_time*)mrb_malloc(mrb, sizeof(*tm));
  *tm2 = *tm;
  tm2->timezone = MRB_TIMEZONE_LOCAL;
  time_update_datetime(mrb, tm2, TRUE);
  return time_wrap(mrb, mrb_obj_class(mrb, self), tm2);
}

/* 15.2.19.7.15 */
/* Returns hour of time. */
static mrb_value
time_hour(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm;

  tm = time_get_ptr(mrb, self);
  return mrb_fixnum_value(tm->datetime.tm_hour);
}

/* 15.2.19.7.16 */
/* Initializes a time by setting the amount of milliseconds since the epoch.*/
static mrb_value
time_init(mrb_state *mrb, mrb_value self)
{
  mrb_int ayear = 0, amonth = 1, aday = 1, ahour = 0,
  amin = 0, asec = 0, ausec = 0;
  mrb_int n;
  struct mrb_time *tm;

  n = mrb_get_args(mrb, "|iiiiiii",
       &ayear, &amonth, &aday, &ahour, &amin, &asec, &ausec);
  tm = (struct mrb_time*)DATA_PTR(self);
  if (tm) {
    mrb_free(mrb, tm);
  }
  mrb_data_init(self, NULL, &time_type);

  if (n == 0) {
    tm = current_mrb_time(mrb);
  }
  else {
    tm = time_mktime(mrb, ayear, amonth, aday, ahour, amin, asec, ausec, MRB_TIMEZONE_LOCAL);
  }
  mrb_data_init(self, tm, &time_type);
  return self;
}

/* 15.2.19.7.17(x) */
/* Initializes a copy of this time object. */
static mrb_value
time_init_copy(mrb_state *mrb, mrb_value copy)
{
  mrb_value src = mrb_get_arg1(mrb);
  struct mrb_time *t1, *t2;

  if (mrb_obj_equal(mrb, copy, src)) return copy;
  if (!mrb_obj_is_instance_of(mrb, src, mrb_obj_class(mrb, copy))) {
    mrb_raise(mrb, E_TYPE_ERROR, "wrong argument class");
  }
  t1 = (struct mrb_time*)DATA_PTR(copy);
  t2 = (struct mrb_time*)DATA_PTR(src);
  if (!t2) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "uninitialized time");
  }
  if (!t1) {
    t1 = (struct mrb_time*)mrb_malloc(mrb, sizeof(struct mrb_time));
    mrb_data_init(copy, t1, &time_type);
  }
  *t1 = *t2;
  return copy;
}

/* 15.2.19.7.18 */
/* Sets the timezone attribute of the Time object to LOCAL. */
static mrb_value
time_localtime(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm;

  tm = time_get_ptr(mrb, self);
  tm->timezone = MRB_TIMEZONE_LOCAL;
  time_update_datetime(mrb, tm, FALSE);
  return self;
}

/* 15.2.19.7.19 */
/* Returns day of month of time. */
static mrb_value
time_mday(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm;

  tm = time_get_ptr(mrb, self);
  return mrb_fixnum_value(tm->datetime.tm_mday);
}

/* 15.2.19.7.20 */
/* Returns minutes of time. */
static mrb_value
time_min(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm;

  tm = time_get_ptr(mrb, self);
  return mrb_fixnum_value(tm->datetime.tm_min);
}

/* 15.2.19.7.21 (mon) and 15.2.19.7.22 (month) */
/* Returns month of time. */
static mrb_value
time_mon(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm;

  tm = time_get_ptr(mrb, self);
  return mrb_fixnum_value(tm->datetime.tm_mon + 1);
}

/* 15.2.19.7.23 */
/* Returns seconds in minute of time. */
static mrb_value
time_sec(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm;

  tm = time_get_ptr(mrb, self);
  return mrb_fixnum_value(tm->datetime.tm_sec);
}

#ifndef MRB_NO_FLOAT
/* 15.2.19.7.24 */
/* Returns a Float with the time since the epoch in seconds. */
static mrb_value
time_to_f(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm;

  tm = time_get_ptr(mrb, self);
  return mrb_float_value(mrb, (mrb_float)tm->sec + (mrb_float)tm->usec/1.0e6);
}
#endif

/* 15.2.19.7.25 */
/* Returns an Integer with the time since the epoch in seconds. */
static mrb_value
time_to_i(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm;

  tm = time_get_ptr(mrb, self);
  return time_value_from_time_t(mrb, tm->sec);
}

/* 15.2.19.7.26 */
/* Returns the number of microseconds for time. */
static mrb_value
time_usec(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm;

  tm = time_get_ptr(mrb, self);
  return mrb_fixnum_value((mrb_int)tm->usec);
}

/* 15.2.19.7.27 */
/* Sets the timezone attribute of the Time object to UTC. */
static mrb_value
time_utc(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm;

  tm = time_get_ptr(mrb, self);
  tm->timezone = MRB_TIMEZONE_UTC;
  time_update_datetime(mrb, tm, FALSE);
  return self;
}

/* 15.2.19.7.28 */
/* Returns true if this time is in the UTC timezone false if not. */
static mrb_value
time_utc_p(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm;

  tm = time_get_ptr(mrb, self);
  return mrb_bool_value(tm->timezone == MRB_TIMEZONE_UTC);
}

static mrb_value
time_to_s(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm = time_get_ptr(mrb, self);
  char buf[64];
  size_t len;

  if (tm->timezone == MRB_TIMEZONE_UTC) {
    len = strftime(buf, sizeof(buf), TO_S_FMT "UTC", &tm->datetime);
  }
  else {
    len = strftime(buf, sizeof(buf), TO_S_FMT, &tm->datetime);
    len += time_zonename(mrb, tm, buf+len, sizeof(buf)-len);
  }
  mrb_value str = mrb_str_new(mrb, buf, len);
  RSTR_SET_ASCII_FLAG(mrb_str_ptr(str));
  return str;
}

static mrb_value
time_hash(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm = time_get_ptr(mrb, self);
  uint32_t hash = mrb_byte_hash((uint8_t*)&tm->sec, sizeof(time_t));
  hash = mrb_byte_hash_step((uint8_t*)&tm->usec, sizeof(time_t), hash);
  hash = mrb_byte_hash_step((uint8_t*)&tm->timezone, sizeof(tm->timezone), hash);
  return mrb_int_value(mrb, hash);
}

#define wday_impl(num) \
  struct mrb_time *tm = time_get_ptr(mrb, self);\
  return mrb_bool_value(tm->datetime.tm_wday == (num));

static mrb_value
time_sunday(mrb_state *mrb, mrb_value self)
{
  wday_impl(0);
}

static mrb_value
time_monday(mrb_state *mrb, mrb_value self)
{
  wday_impl(1);
}

static mrb_value
time_tuesday(mrb_state *mrb, mrb_value self)
{
  wday_impl(2);
}

static mrb_value
time_wednesday(mrb_state *mrb, mrb_value self)
{
  wday_impl(3);
}

static mrb_value
time_thursday(mrb_state *mrb, mrb_value self)
{
  wday_impl(4);
}

static mrb_value
time_friday(mrb_state *mrb, mrb_value self)
{
  wday_impl(5);
}

static mrb_value
time_saturday(mrb_state *mrb, mrb_value self)
{
  wday_impl(6);
}

void
mrb_mruby_time_gem_init(mrb_state* mrb)
{
  struct RClass *tc;
  /* ISO 15.2.19.2 */
  tc = mrb_define_class(mrb, "Time", mrb->object_class);
  MRB_SET_INSTANCE_TT(tc, MRB_TT_CDATA);
  mrb_include_module(mrb, tc, mrb_module_get(mrb, "Comparable"));
  mrb_define_class_method(mrb, tc, "at", time_at_m, MRB_ARGS_ARG(1, 1));    /* 15.2.19.6.1 */
  mrb_define_class_method(mrb, tc, "gm", time_gm, MRB_ARGS_ARG(1,6));       /* 15.2.19.6.2 */
  mrb_define_class_method(mrb, tc, "local", time_local, MRB_ARGS_ARG(1,6)); /* 15.2.19.6.3 */
  mrb_define_class_method(mrb, tc, "mktime", time_local, MRB_ARGS_ARG(1,6));/* 15.2.19.6.4 */
  mrb_define_class_method(mrb, tc, "now", time_now, MRB_ARGS_NONE());       /* 15.2.19.6.5 */
  mrb_define_class_method(mrb, tc, "utc", time_gm, MRB_ARGS_ARG(1,6));      /* 15.2.19.6.6 */

  mrb_define_method(mrb, tc, "hash"   , time_hash   , MRB_ARGS_NONE());
  mrb_define_method(mrb, tc, "eql?"   , time_eq     , MRB_ARGS_REQ(1));
  mrb_define_method(mrb, tc, "=="     , time_eq     , MRB_ARGS_REQ(1));
  mrb_define_method(mrb, tc, "<=>"    , time_cmp    , MRB_ARGS_REQ(1)); /* 15.2.19.7.1 */
  mrb_define_method(mrb, tc, "+"      , time_plus   , MRB_ARGS_REQ(1)); /* 15.2.19.7.2 */
  mrb_define_method(mrb, tc, "-"      , time_minus  , MRB_ARGS_REQ(1)); /* 15.2.19.7.3 */
  mrb_define_method(mrb, tc, "to_s"   , time_to_s   , MRB_ARGS_NONE());
  mrb_define_method(mrb, tc, "inspect", time_to_s   , MRB_ARGS_NONE());
  mrb_define_method(mrb, tc, "asctime", time_asctime, MRB_ARGS_NONE()); /* 15.2.19.7.4 */
  mrb_define_method(mrb, tc, "ctime"  , time_asctime, MRB_ARGS_NONE()); /* 15.2.19.7.5 */
  mrb_define_method(mrb, tc, "day"    , time_day    , MRB_ARGS_NONE()); /* 15.2.19.7.6 */
  mrb_define_method(mrb, tc, "dst?"   , time_dst_p  , MRB_ARGS_NONE()); /* 15.2.19.7.7 */
  mrb_define_method(mrb, tc, "getgm"  , time_getutc , MRB_ARGS_NONE()); /* 15.2.19.7.8 */
  mrb_define_method(mrb, tc, "getlocal",time_getlocal,MRB_ARGS_NONE()); /* 15.2.19.7.9 */
  mrb_define_method(mrb, tc, "getutc" , time_getutc , MRB_ARGS_NONE()); /* 15.2.19.7.10 */
  mrb_define_method(mrb, tc, "gmt?"   , time_utc_p  , MRB_ARGS_NONE()); /* 15.2.19.7.11 */
  mrb_define_method(mrb, tc, "gmtime" , time_utc    , MRB_ARGS_NONE()); /* 15.2.19.7.13 */
  mrb_define_method(mrb, tc, "hour"   , time_hour, MRB_ARGS_NONE());    /* 15.2.19.7.15 */
  mrb_define_method(mrb, tc, "localtime", time_localtime, MRB_ARGS_NONE()); /* 15.2.19.7.18 */
  mrb_define_method(mrb, tc, "mday"   , time_mday, MRB_ARGS_NONE());    /* 15.2.19.7.19 */
  mrb_define_method(mrb, tc, "min"    , time_min, MRB_ARGS_NONE());     /* 15.2.19.7.20 */

  mrb_define_method(mrb, tc, "mon"  , time_mon, MRB_ARGS_NONE());       /* 15.2.19.7.21 */
  mrb_define_method(mrb, tc, "month", time_mon, MRB_ARGS_NONE());       /* 15.2.19.7.22 */

  mrb_define_method(mrb, tc, "sec" , time_sec, MRB_ARGS_NONE());        /* 15.2.19.7.23 */
  mrb_define_method(mrb, tc, "to_i", time_to_i, MRB_ARGS_NONE());       /* 15.2.19.7.25 */
#ifndef MRB_NO_FLOAT
  mrb_define_method(mrb, tc, "to_f", time_to_f, MRB_ARGS_NONE());       /* 15.2.19.7.24 */
#endif
  mrb_define_method(mrb, tc, "usec", time_usec, MRB_ARGS_NONE());       /* 15.2.19.7.26 */
  mrb_define_method(mrb, tc, "utc" , time_utc, MRB_ARGS_NONE());        /* 15.2.19.7.27 */
  mrb_define_method(mrb, tc, "utc?", time_utc_p,MRB_ARGS_NONE());       /* 15.2.19.7.28 */
  mrb_define_method(mrb, tc, "wday", time_wday, MRB_ARGS_NONE());       /* 15.2.19.7.30 */
  mrb_define_method(mrb, tc, "yday", time_yday, MRB_ARGS_NONE());       /* 15.2.19.7.31 */
  mrb_define_method(mrb, tc, "year", time_year, MRB_ARGS_NONE());       /* 15.2.19.7.32 */
  mrb_define_method(mrb, tc, "zone", time_zone, MRB_ARGS_NONE());       /* 15.2.19.7.33 */

  mrb_define_method(mrb, tc, "initialize", time_init, MRB_ARGS_REQ(1)); /* 15.2.19.7.16 */
  mrb_define_method(mrb, tc, "initialize_copy", time_init_copy, MRB_ARGS_REQ(1)); /* 15.2.19.7.17 */

  mrb_define_method(mrb, tc, "sunday?", time_sunday, MRB_ARGS_NONE());
  mrb_define_method(mrb, tc, "monday?", time_monday, MRB_ARGS_NONE());
  mrb_define_method(mrb, tc, "tuesday?", time_tuesday, MRB_ARGS_NONE());
  mrb_define_method(mrb, tc, "wednesday?", time_wednesday, MRB_ARGS_NONE());
  mrb_define_method(mrb, tc, "thursday?", time_thursday, MRB_ARGS_NONE());
  mrb_define_method(mrb, tc, "friday?", time_friday, MRB_ARGS_NONE());
  mrb_define_method(mrb, tc, "saturday?", time_saturday, MRB_ARGS_NONE());

  /*
    methods not available:
      gmt_offset(15.2.19.7.12)
      gmtoff(15.2.19.7.14)
      utc_offset(15.2.19.7.29)
  */
}

void
mrb_mruby_time_gem_final(mrb_state* mrb)
{
}
