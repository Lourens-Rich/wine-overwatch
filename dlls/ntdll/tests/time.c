/*
 * Unit test suite for ntdll time functions
 *
 * Copyright 2004 Rein Klazes
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define NONAMELESSUNION
#include "ntdll_test.h"
#include "ddk/wdm.h"

#define TICKSPERSEC        10000000
#define TICKSPERMSEC       10000
#define SECSPERDAY         86400

static VOID (WINAPI *pRtlTimeToTimeFields)( const LARGE_INTEGER *liTime, PTIME_FIELDS TimeFields) ;
static VOID (WINAPI *pRtlTimeFieldsToTime)(  PTIME_FIELDS TimeFields,  PLARGE_INTEGER Time) ;
static NTSTATUS (WINAPI *pNtDelayExecution)(BOOLEAN alterable, const LARGE_INTEGER *timeout);
static ULONG (WINAPI *pNtGetTickCount)(void);
static void (WINAPI *pNtQuerySystemTime)(PLARGE_INTEGER Time);

static const int MonthLengths[2][12] =
{
	{ 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },
	{ 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }
};

static inline BOOL IsLeapYear(int Year)
{
    return Year % 4 == 0 && (Year % 100 != 0 || Year % 400 == 0);
}

/* start time of the tests */
static TIME_FIELDS tftest = {1889,12,31,23,59,59,0,0};

static void test_pRtlTimeToTimeFields(void)
{
    LARGE_INTEGER litime , liresult;
    TIME_FIELDS tfresult;
    int i=0;
    litime.QuadPart = ((ULONGLONG)0x0144017a << 32) | 0xf0b0a980;
    while( tftest.Year < 2110 ) {
        /* test at the last second of the month */
        pRtlTimeToTimeFields( &litime, &tfresult);
        ok( tfresult.Year == tftest.Year && tfresult.Month == tftest.Month &&
            tfresult.Day == tftest.Day && tfresult.Hour == tftest.Hour && 
            tfresult.Minute == tftest.Minute && tfresult.Second == tftest.Second,
            "#%d expected: %d-%d-%d %d:%d:%d  got:  %d-%d-%d %d:%d:%d\n", ++i,
            tftest.Year, tftest.Month, tftest.Day,
            tftest.Hour, tftest.Minute,tftest.Second,
            tfresult.Year, tfresult.Month, tfresult.Day,
            tfresult.Hour, tfresult.Minute, tfresult.Second);
        /* test the inverse */
        pRtlTimeFieldsToTime( &tfresult, &liresult);
        ok( liresult.QuadPart == litime.QuadPart," TimeFieldsToTime failed on %d-%d-%d %d:%d:%d. Error is %d ticks\n",
            tfresult.Year, tfresult.Month, tfresult.Day,
            tfresult.Hour, tfresult.Minute, tfresult.Second,
            (int) (liresult.QuadPart - litime.QuadPart) );
        /*  one second later is beginning of next month */
        litime.QuadPart +=  TICKSPERSEC ;
        pRtlTimeToTimeFields( &litime, &tfresult);
        ok( tfresult.Year == tftest.Year + (tftest.Month ==12) &&
            tfresult.Month == tftest.Month % 12 + 1 &&
            tfresult.Day == 1 && tfresult.Hour == 0 && 
            tfresult.Minute == 0 && tfresult.Second == 0,
            "#%d expected: %d-%d-%d %d:%d:%d  got:  %d-%d-%d %d:%d:%d\n", ++i,
            tftest.Year + (tftest.Month ==12),
            tftest.Month % 12 + 1, 1, 0, 0, 0,
            tfresult.Year, tfresult.Month, tfresult.Day,
            tfresult.Hour, tfresult.Minute, tfresult.Second);
        /* test the inverse */
        pRtlTimeFieldsToTime( &tfresult, &liresult);
        ok( liresult.QuadPart == litime.QuadPart," TimeFieldsToTime failed on %d-%d-%d %d:%d:%d. Error is %d ticks\n",
            tfresult.Year, tfresult.Month, tfresult.Day,
            tfresult.Hour, tfresult.Minute, tfresult.Second,
            (int) (liresult.QuadPart - litime.QuadPart) );
        /* advance to the end of the month */
        litime.QuadPart -=  TICKSPERSEC ;
        if( tftest.Month == 12) {
            tftest.Month = 1;
            tftest.Year += 1;
        } else 
            tftest.Month += 1;
        tftest.Day = MonthLengths[IsLeapYear(tftest.Year)][tftest.Month - 1];
        litime.QuadPart +=  (LONGLONG) tftest.Day * TICKSPERSEC * SECSPERDAY;
    }
}

static void test_TickCount(void)
{
    /* This is a well-known address relied upon by programs. */
    PKSHARED_USER_DATA user_shared_data = (void *)0x7ffe0000;
    LARGE_INTEGER now;
    LONG diff;

    Sleep(250);

    /* Ideally, this would be continuously updated. */
    diff = user_shared_data->u.TickCountQuad;
    diff = NtGetTickCount() - diff;
    todo_wine ok( diff < 16,
        "NtGetTickCount - TickCountQuad too high, expected: < 16  got: %d\n", diff);

    /* We try to do good enough and have NtQuerySystemTime reinitialize user_shared_data. */
    NtQuerySystemTime( &now );
    diff = user_shared_data->u.TickCountQuad;
    diff = NtGetTickCount() - diff;
    ok( diff < 16,
        "NtGetTickCount - TickCountQuad too high, expected: < 16  got: %d\n", diff);

    NtQuerySystemTime( &now );
    diff = (user_shared_data->TickCountLowDeprecated * (LONGLONG)user_shared_data->TickCountMultiplier) >> 24;
    diff = NtGetTickCount() - diff;
    ok( diff < 16,
        "NtGetTickCount - TickCountLow*TickCountMultiplier too high, expected: < 16  got: %d\n",
        diff);
}

START_TEST(time)
{
    HMODULE mod = GetModuleHandleA("ntdll.dll");
    pRtlTimeToTimeFields = (void *)GetProcAddress(mod,"RtlTimeToTimeFields");
    pRtlTimeFieldsToTime = (void *)GetProcAddress(mod,"RtlTimeFieldsToTime");
    if (pRtlTimeToTimeFields && pRtlTimeFieldsToTime)
        test_pRtlTimeToTimeFields();
    else
        win_skip("Required time conversion functions are not available\n");

    pNtDelayExecution = (void *)GetProcAddress(mod,"NtDelayExecution");
    pNtGetTickCount = (void *)GetProcAddress(mod,"NtGetTickCount");
    pNtQuerySystemTime = (void *)GetProcAddress(mod,"NtQuerySystemTime");
    test_TickCount();
}
