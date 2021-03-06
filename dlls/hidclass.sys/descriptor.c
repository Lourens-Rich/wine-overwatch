/*
 * HID descriptor parsing
 *
 * Copyright (C) 2015 Aric Stewart
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

#include "config.h"

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#define NONAMELESSUNION
#include "hid.h"

#include "wine/debug.h"
#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(hid);

#define USAGE_MAX 10

/* Flags that are defined in the document
   "Device Class Definition for Human Interface Devices" */
enum {
    INPUT_DATA_CONST = 0x01, /* Data (0)             | Constant (1)       */
    INPUT_ARRAY_VAR = 0x02,  /* Array (0)            | Variable (1)       */
    INPUT_ABS_REL = 0x04,    /* Absolute (0)         | Relative (1)       */
    INPUT_WRAP = 0x08,       /* No Wrap (0)          | Wrap (1)           */
    INPUT_LINEAR = 0x10,     /* Linear (0)           | Non Linear (1)     */
    INPUT_PREFSTATE = 0x20,  /* Preferred State (0)  | No Preferred (1)   */
    INPUT_NULL = 0x40,       /* No Null position (0) | Null state(1)      */
    INPUT_VOLATILE = 0x80,   /* Non Volatile (0)     | Volatile (1)       */
    INPUT_BITFIELD = 0x100   /* Bit Field (0)        | Buffered Bytes (1) */
};

enum {
    TAG_TYPE_MAIN = 0x0,
    TAG_TYPE_GLOBAL,
    TAG_TYPE_LOCAL,
    TAG_TYPE_RESERVED,
};

enum {
    TAG_MAIN_INPUT = 0x08,
    TAG_MAIN_OUTPUT = 0x09,
    TAG_MAIN_FEATURE = 0x0B,
    TAG_MAIN_COLLECTION = 0x0A,
    TAG_MAIN_END_COLLECTION = 0x0C
};

enum {
    TAG_GLOBAL_USAGE_PAGE = 0x0,
    TAG_GLOBAL_LOGICAL_MINIMUM,
    TAG_GLOBAL_LOGICAL_MAXIMUM,
    TAG_GLOBAL_PHYSICAL_MINIMUM,
    TAG_GLOBAL_PHYSICAL_MAXIMUM,
    TAG_GLOBAL_UNIT_EXPONENT,
    TAG_GLOBAL_UNIT,
    TAG_GLOBAL_REPORT_SIZE,
    TAG_GLOBAL_REPORT_ID,
    TAG_GLOBAL_REPORT_COUNT,
    TAG_GLOBAL_PUSH,
    TAG_GLOBAL_POP
};

enum {
    TAG_LOCAL_USAGE = 0x0,
    TAG_LOCAL_USAGE_MINIMUM,
    TAG_LOCAL_USAGE_MAXIMUM,
    TAG_LOCAL_DESIGNATOR_INDEX,
    TAG_LOCAL_DESIGNATOR_MINIMUM,
    TAG_LOCAL_DESIGNATOR_MAXIMUM,
    TAG_LOCAL_STRING_INDEX,
    TAG_LOCAL_STRING_MINIMUM,
    TAG_LOCAL_STRING_MAXIMUM,
    TAG_LOCAL_DELIMITER
};


static const char* const feature_string[] =
    { "Input", "Output", "Feature" };

struct caps {
    USAGE UsagePage;
    LONG LogicalMin;
    LONG LogicalMax;
    LONG PhysicalMin;
    LONG PhysicalMax;
    ULONG UnitsExp;
    ULONG Units;
    USHORT BitSize;
    UCHAR ReportID;
    USHORT ReportCount;

    BOOLEAN  IsRange;
    BOOLEAN  IsStringRange;
    BOOLEAN  IsDesignatorRange;
    unsigned int usage_count;
    union {
        struct {
            USAGE UsageMin;
            USAGE UsageMax;
            USHORT StringMin;
            USHORT StringMax;
            USHORT DesignatorMin;
            USHORT DesignatorMax;
        } Range;
        struct  {
            USAGE Usage[USAGE_MAX];
            USAGE Reserved1;
            USHORT StringIndex;
            USHORT Reserved2;
            USHORT DesignatorIndex;
            USHORT Reserved3;
        } NotRange;
    } DUMMYUNIONNAME;

    int Delim;
};

struct feature {
    struct list entry;
    struct list col_entry;
    struct caps caps;

    HIDP_REPORT_TYPE type;
    BOOLEAN isData;
    BOOLEAN isArray;
    BOOLEAN IsAbsolute;
    BOOLEAN Wrap;
    BOOLEAN Linear;
    BOOLEAN prefState;
    BOOLEAN HasNull;
    BOOLEAN Volatile;
    BOOLEAN BitField;

    unsigned int index;
    struct collection *collection;
};

static const char* const collection_string[] = {
    "Physical",
    "Application",
    "Logical",
    "Report",
    "Named Array",
    "Usage Switch",
    "Usage Modifier",
};

struct collection {
    struct list entry;
    struct caps caps;
    unsigned int index;
    unsigned int type;
    struct collection *parent;
    struct list features;
    struct list collections;
};

struct caps_stack {
    struct list entry;
    struct caps caps;
};

static const char* debugstr_usages(struct caps *caps)
{
    if (!caps->IsRange)
    {
        char out[12 * USAGE_MAX];
        unsigned int i;
        if (caps->usage_count == 0)
            return "[ none ]";
        out[0] = 0;
        for (i = 0; i < caps->usage_count; i++)
            sprintf(out + strlen(out), "0x%x ", caps->u.NotRange.Usage[i]);
        return wine_dbg_sprintf("[ %s] ", out);
    }
    else
        return wine_dbg_sprintf("[0x%x - 0x%x]", caps->u.Range.UsageMin, caps->u.Range.UsageMax);
}

static const char* debugstr_stringindex(struct caps *caps)
{
    if (!caps->IsStringRange)
        return wine_dbg_sprintf("%i", caps->u.NotRange.StringIndex);
    else
        return wine_dbg_sprintf("[%i - %i]", caps->u.Range.StringMin, caps->u.Range.StringMax);
}

static const char* debugstr_designatorindex(struct caps *caps)
{
    if (!caps->IsDesignatorRange)
        return wine_dbg_sprintf("%i", caps->u.NotRange.DesignatorIndex);
    else
        return wine_dbg_sprintf("[%i - %i]", caps->u.Range.DesignatorMin, caps->u.Range.DesignatorMax);
}

static void debugstr_caps(const char* type, struct caps *caps)
{
    if (!caps)
        return;
    TRACE("(%s Caps: UsagePage 0x%x; LogicalMin %i; LogicalMax %i; PhysicalMin %i; PhysicalMax %i; UnitsExp %i; Units %i; BitSize %i; ReportID %i; ReportCount %i; Usage %s; StringIndex %s; DesignatorIndex %s; Delim %i;)\n",
    type,
    caps->UsagePage,
    caps->LogicalMin,
    caps->LogicalMax,
    caps->PhysicalMin,
    caps->PhysicalMax,
    caps->UnitsExp,
    caps->Units,
    caps->BitSize,
    caps->ReportID,
    caps->ReportCount,
    debugstr_usages(caps),
    debugstr_stringindex(caps),
    debugstr_designatorindex(caps),
    caps->Delim);
}

static void debug_feature(struct feature *feature)
{
    if (!feature)
        return;
    TRACE("[Feature type %s [%i]; %s; %s; %s; %s; %s; %s; %s; %s; %s]\n",
    feature_string[feature->type],
    feature->index,
    (feature->isData)?"Data":"Const",
    (feature->isArray)?"Array":"Var",
    (feature->IsAbsolute)?"Abs":"Rel",
    (feature->Wrap)?"Wrap":"NoWrap",
    (feature->Linear)?"Linear":"NonLinear",
    (feature->prefState)?"PrefStat":"NoPrefState",
    (feature->HasNull)?"HasNull":"NoNull",
    (feature->Volatile)?"Volatile":"NonVolatile",
    (feature->BitField)?"BitField":"Buffered");

    debugstr_caps("Feature", &feature->caps);
}

static void debug_collection(struct collection *collection)
{
    struct feature *fentry;
    struct collection *centry;
    if (TRACE_ON(hid))
    {
        TRACE("START Collection %i <<< %s, parent: %p,  %i features,  %i collections\n", collection->index, collection_string[collection->type], collection->parent, list_count(&collection->features), list_count(&collection->collections));
        debugstr_caps("Collection", &collection->caps);
        LIST_FOR_EACH_ENTRY(fentry, &collection->features, struct feature, col_entry)
            debug_feature(fentry);
        LIST_FOR_EACH_ENTRY(centry, &collection->collections, struct collection, entry)
            debug_collection(centry);
        TRACE(">>> END Collection %i\n", collection->index);
    }
}

static void debug_print_button_cap(const CHAR * type, WINE_HID_ELEMENT *wine_element)
{
    if (!wine_element->caps.button.IsRange)
        TRACE("%s Button: 0x%x/0x%04x: ReportId %i, startBit %i/1\n" , type,
            wine_element->caps.button.UsagePage,
            wine_element->caps.button.u.NotRange.Usage,
            wine_element->caps.value.ReportID,
            wine_element->valueStartBit);
    else
        TRACE("%s Button: 0x%x/[0x%04x-0x%04x]: ReportId %i, startBit %i/%i\n" ,type,
               wine_element->caps.button.UsagePage,
               wine_element->caps.button.u.Range.UsageMin,
               wine_element->caps.button.u.Range.UsageMax,
               wine_element->caps.value.ReportID,
               wine_element->valueStartBit,
               wine_element->bitCount);
}

static void debug_print_value_cap(const CHAR * type, WINE_HID_ELEMENT *wine_element)
{
    TRACE("%s Value: 0x%x/0x%x: ReportId %i, IsAbsolute %i, HasNull %i, "
          "Bit Size %i, ReportCount %i, UnitsExp %i, Units %i, "
          "LogicalMin %i, Logical Max %i, PhysicalMin %i, "
          "PhysicalMax %i -- StartBit %i/%i\n", type,
            wine_element->caps.value.UsagePage,
            wine_element->caps.value.u.NotRange.Usage,
            wine_element->caps.value.ReportID,
            wine_element->caps.value.IsAbsolute,
            wine_element->caps.value.HasNull,
            wine_element->caps.value.BitSize,
            wine_element->caps.value.ReportCount,
            wine_element->caps.value.UnitsExp,
            wine_element->caps.value.Units,
            wine_element->caps.value.LogicalMin,
            wine_element->caps.value.LogicalMax,
            wine_element->caps.value.PhysicalMin,
            wine_element->caps.value.PhysicalMax,
            wine_element->valueStartBit,
            wine_element->bitCount);
}

static void debug_print_element(const CHAR* type, WINE_HID_ELEMENT *wine_element)
{
    if (wine_element->ElementType == ButtonElement)
        debug_print_button_cap(type, wine_element);
    else if (wine_element->ElementType == ValueElement)
        debug_print_value_cap(type, wine_element);
    else
        TRACE("%s: UNKNOWN\n", type);
}

static void debug_print_report(const char* type, WINE_HID_REPORT *report)
{
    unsigned int i;
    TRACE("START Report %i <<< %s report : dwSize: %i elementCount: %i\n",
        report->reportID,
        type,
        report->dwSize,
        report->elementCount);
    for (i = 0; i < report->elementCount; i++)
        debug_print_element(type, &report->Elements[i]);
    TRACE(">>> END Report %i\n",report->reportID);
}

static void debug_print_preparsed(WINE_HIDP_PREPARSED_DATA *data)
{
    unsigned int i;
    WINE_HID_REPORT *r;
    if (TRACE_ON(hid))
    {
        TRACE("START PREPARSED Data <<< dwSize: %i Usage: %i, UsagePage: %i, InputReportByteLength: %i, tOutputReportByteLength: %i, FeatureReportByteLength: %i, NumberLinkCollectionNodes: %i, NumberInputButtonCaps: %i, NumberInputValueCaps: %i,NumberInputDataIndices: %i, NumberOutputButtonCaps: %i, NumberOutputValueCaps: %i, NumberOutputDataIndices: %i, NumberFeatureButtonCaps: %i, NumberFeatureValueCaps: %i, NumberFeatureDataIndices: %i, dwInputReportCount: %i, dwOutputReportCount: %i, dwFeatureReportCount: %i, dwOutputReportOffset: %i, dwFeatureReportOffset: %i\n",
        data->dwSize,
        data->caps.Usage,
        data->caps.UsagePage,
        data->caps.InputReportByteLength,
        data->caps.OutputReportByteLength,
        data->caps.FeatureReportByteLength,
        data->caps.NumberLinkCollectionNodes,
        data->caps.NumberInputButtonCaps,
        data->caps.NumberInputValueCaps,
        data->caps.NumberInputDataIndices,
        data->caps.NumberOutputButtonCaps,
        data->caps.NumberOutputValueCaps,
        data->caps.NumberOutputDataIndices,
        data->caps.NumberFeatureButtonCaps,
        data->caps.NumberFeatureValueCaps,
        data->caps.NumberFeatureDataIndices,
        data->dwInputReportCount,
        data->dwOutputReportCount,
        data->dwFeatureReportCount,
        data->dwOutputReportOffset,
        data->dwFeatureReportOffset);

        r = HID_INPUT_REPORTS(data);
        for (i = 0; i < data->dwInputReportCount; i++)
        {
            debug_print_report("INPUT", r);
            r = HID_NEXT_REPORT(data, r);
        }
        r = HID_OUTPUT_REPORTS(data);
        for (i = 0; i < data->dwOutputReportCount; i++)
        {
            debug_print_report("OUTPUT", r);
            r = HID_NEXT_REPORT(data, r);
        }
        r = HID_FEATURE_REPORTS(data);
        for (i = 0; i < data->dwFeatureReportCount; i++)
        {
            debug_print_report("FEATURE", r);
            r = HID_NEXT_REPORT(data, r);
        }
        TRACE(">>> END Preparsed Data\n");
    }
}

static int getValue(int bsize, int source, BOOL allow_negative)
{
    int mask = 0xff;
    int negative = 0x80;
    int outofrange = 0x100;
    int value;
    unsigned int i;

    if (bsize == 4)
        return source;

    for (i = 1; i < bsize; i++)
    {
        mask = (mask<<8) + 0xff;
        negative = (negative<<8);
        outofrange = (outofrange<<8);
    }
    value = (source&mask);
    if (allow_negative && value&negative)
        value = -1 * (outofrange - value);
    return value;
}

static void parse_io_feature(unsigned int bSize, int itemVal, int bTag,
                             unsigned int *feature_index,
                             struct feature *feature)
{
    if (bSize <= 0)
    {
        return;
    }
    else
    {
        feature->isData = ((itemVal & INPUT_DATA_CONST) == 0);
        feature->isArray =  ((itemVal & INPUT_ARRAY_VAR) == 0);
        feature->IsAbsolute = ((itemVal & INPUT_ABS_REL) == 0);
        feature->Wrap = ((itemVal & INPUT_WRAP) != 0);
        feature->Linear = ((itemVal & INPUT_LINEAR) == 0);
        feature->prefState = ((itemVal & INPUT_PREFSTATE) == 0);
        feature->HasNull = ((itemVal & INPUT_NULL) != 0);

        if (bTag != TAG_MAIN_INPUT)
        {
            feature->Volatile = ((itemVal & INPUT_VOLATILE) != 0);
        }
        if (bSize > 1)
        {
            feature->BitField = ((itemVal & INPUT_BITFIELD) == 0);
        }
        feature->index = *feature_index;
        *feature_index = *feature_index + 1;
    }
}

static void parse_collection(unsigned int bSize, int itemVal,
                             struct collection *collection)
{
    if (bSize <= 0)
        return;
    else
     {
        collection->type = itemVal;

        if (itemVal >= 0x07 && itemVal <= 0x7F) {
            ERR(" (Reserved 0x%x )\n", itemVal);
        }
        else if (itemVal >= 0x80 && itemVal <= 0xFF) {
            ERR(" (Vendor Defined 0x%x )\n", itemVal);
        }
    }
}

static void new_caps(struct caps *caps)
{
    caps->IsRange = 0;
    caps->IsStringRange = 0;
    caps->IsDesignatorRange = 0;
    caps->usage_count = 0;
}

static int parse_descriptor(BYTE *descriptor, unsigned int index, unsigned int length,
                            unsigned int *feature_index, unsigned int *collection_index,
                            struct collection *collection, struct caps *caps,
                            struct list *features, struct list *stack)
{
    unsigned int i;
    for (i = index; i < length;)
    {
        BYTE b0 = descriptor[i++];
        int bSize = b0 & 0x03;
        int bType = (b0 >> 2) & 0x03;
        int bTag = (b0 >> 4) & 0x0F;

        bSize = (bSize == 3) ? 4 : bSize;
        if (bType == TAG_TYPE_RESERVED && bTag == 0x0F && bSize == 2 &&
            i + 2 < length)
        {
            /* Long data items: Should be unused */
            ERR("Long Data Item, should be unused\n");
        }
        else
        {
            int bSizeActual = 0;
            int itemVal = 0;
            unsigned int j;

            for (j = 0; j < bSize; j++)
            {
                if (i + j < length)
                {
                    itemVal += descriptor[i + j] << (8 * j);
                    bSizeActual++;
                }
            }
            TRACE(" 0x%x[%i], type %i , tag %i, size %i, val %i\n",b0,i-1,bType, bTag, bSize, itemVal );

            if (bType == TAG_TYPE_MAIN)
            {
                struct feature *feature;
                switch(bTag)
                {
                    case TAG_MAIN_INPUT:
                        feature = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*feature));
                        list_add_tail(&collection->features, &feature->col_entry);
                        list_add_tail(features, &feature->entry);
                        feature->type = HidP_Input;
                        parse_io_feature(bSize, itemVal, bTag, feature_index, feature);
                        feature->caps = *caps;
                        feature->collection = collection;
                        new_caps(caps);
                        break;
                    case TAG_MAIN_OUTPUT:
                        feature = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*feature));
                        list_add_tail(&collection->features, &feature->col_entry);
                        list_add_tail(features, &feature->entry);
                        feature->type = HidP_Output;
                        parse_io_feature(bSize, itemVal, bTag, feature_index, feature);
                        feature->caps = *caps;
                        feature->collection = collection;
                        new_caps(caps);
                        break;
                    case TAG_MAIN_FEATURE:
                        feature = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*feature));
                        list_add_tail(&collection->features, &feature->col_entry);
                        list_add_tail(features, &feature->entry);
                        feature->type = HidP_Feature;
                        parse_io_feature(bSize, itemVal, bTag, feature_index, feature);
                        feature->caps = *caps;
                        feature->collection = collection;
                        new_caps(caps);
                        break;
                    case TAG_MAIN_COLLECTION:
                    {
                        struct collection *subcollection = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(struct collection));
                        list_add_tail(&collection->collections, &subcollection->entry);
                        subcollection->parent = collection;
                        /* Only set our collection once...
                           We do not properly handle composite devices yet. */
                        if (*collection_index == 0)
                            collection->caps = *caps;
                        subcollection->caps = *caps;
                        subcollection->index = *collection_index;
                        *collection_index = *collection_index + 1;
                        list_init(&subcollection->features);
                        list_init(&subcollection->collections);
                        new_caps(caps);

                        parse_collection(bSize, itemVal, subcollection);

                        i = parse_descriptor(descriptor, i+1, length, feature_index, collection_index, subcollection, caps, features, stack);
                        continue;
                    }
                    case TAG_MAIN_END_COLLECTION:
                        return i;
                    default:
                        ERR("Unknown (bTag: 0x%x, bType: 0x%x)\n", bTag, bType);
                }
            }
            else if (bType == TAG_TYPE_GLOBAL)
            {
                switch(bTag)
                {
                    case TAG_GLOBAL_USAGE_PAGE:
                        caps->UsagePage = getValue(bSize, itemVal, FALSE);
                        break;
                    case TAG_GLOBAL_LOGICAL_MINIMUM:
                        caps->LogicalMin = getValue(bSize, itemVal, TRUE);
                        break;
                    case TAG_GLOBAL_LOGICAL_MAXIMUM:
                        caps->LogicalMax = getValue(bSize, itemVal, TRUE);
                        break;
                    case TAG_GLOBAL_PHYSICAL_MINIMUM:
                        caps->PhysicalMin = getValue(bSize, itemVal, TRUE);
                        break;
                    case TAG_GLOBAL_PHYSICAL_MAXIMUM:
                        caps->PhysicalMax = getValue(bSize, itemVal, TRUE);
                        break;
                    case TAG_GLOBAL_UNIT_EXPONENT:
                        caps->UnitsExp = getValue(bSize, itemVal, TRUE);
                        break;
                    case TAG_GLOBAL_UNIT:
                        caps->Units = getValue(bSize, itemVal, TRUE);
                        break;
                    case TAG_GLOBAL_REPORT_SIZE:
                        caps->BitSize = getValue(bSize, itemVal, FALSE);
                        break;
                    case TAG_GLOBAL_REPORT_ID:
                        caps->ReportID = getValue(bSize, itemVal, FALSE);
                        break;
                    case TAG_GLOBAL_REPORT_COUNT:
                        caps->ReportCount = getValue(bSize, itemVal, FALSE);
                        break;
                    case TAG_GLOBAL_PUSH:
                    {
                        struct caps_stack *saved = HeapAlloc(GetProcessHeap(), 0, sizeof(*saved));
                        saved->caps = *caps;
                        TRACE("Push\n");
                        list_add_tail(stack, &saved->entry);
                        break;
                    }
                    case TAG_GLOBAL_POP:
                    {
                        struct list *tail;
                        struct caps_stack *saved;
                        TRACE("Pop\n");
                        tail = list_tail(stack);
                        if (tail)
                        {
                            saved = LIST_ENTRY(tail, struct caps_stack, entry);
                            *caps = saved->caps;
                            list_remove(tail);
                            HeapFree(GetProcessHeap(), 0, saved);
                        }
                        else
                            ERR("Pop but no stack!\n");
                        break;
                    }
                    default:
                        ERR("Unknown (bTag: 0x%x, bType: 0x%x)\n", bTag, bType);
                }
            }
            else if (bType == TAG_TYPE_LOCAL)
            {
                switch(bTag)
                {
                    case TAG_LOCAL_USAGE:
                        if (caps->usage_count >= USAGE_MAX)
                            FIXME("More than %i individual usages defined\n",USAGE_MAX);
                        else
                        {
                            caps->u.NotRange.Usage[caps->usage_count++] = getValue(bSize, itemVal, FALSE);
                            caps->IsRange = FALSE;
                        }
                        break;
                    case TAG_LOCAL_USAGE_MINIMUM:
                        caps->usage_count = 1;
                        caps->u.Range.UsageMin = getValue(bSize, itemVal, FALSE);
                        caps->IsRange = TRUE;
                        break;
                    case TAG_LOCAL_USAGE_MAXIMUM:
                        caps->usage_count = 1;
                        caps->u.Range.UsageMax = getValue(bSize, itemVal, FALSE);
                        caps->IsRange = TRUE;
                        break;
                    case TAG_LOCAL_DESIGNATOR_INDEX:
                        caps->u.NotRange.DesignatorIndex = getValue(bSize, itemVal, FALSE);
                        caps->IsDesignatorRange = FALSE;
                        break;
                    case TAG_LOCAL_DESIGNATOR_MINIMUM:
                        caps->u.Range.DesignatorMin = getValue(bSize, itemVal, FALSE);
                        caps->IsDesignatorRange = TRUE;
                        break;
                    case TAG_LOCAL_DESIGNATOR_MAXIMUM:
                        caps->u.Range.DesignatorMax = getValue(bSize, itemVal, FALSE);
                        caps->IsDesignatorRange = TRUE;
                        break;
                    case TAG_LOCAL_STRING_INDEX:
                        caps->u.NotRange.StringIndex = getValue(bSize, itemVal, FALSE);
                        caps->IsStringRange = FALSE;
                        break;
                    case TAG_LOCAL_STRING_MINIMUM:
                        caps->u.Range.StringMin = getValue(bSize, itemVal, FALSE);
                        caps->IsStringRange = TRUE;
                        break;
                    case TAG_LOCAL_STRING_MAXIMUM:
                        caps->u.Range.StringMax = getValue(bSize, itemVal, FALSE);
                        caps->IsStringRange = TRUE;
                        break;
                    case TAG_LOCAL_DELIMITER:
                        caps->Delim = getValue(bSize, itemVal, FALSE);
                        break;
                    default:
                        ERR("Unknown (bTag: 0x%x, bType: 0x%x)\n", bTag, bType);
                }
            }
            else
                ERR("Unknown (bTag: 0x%x, bType: 0x%x)\n", bTag, bType);

            i += bSize;
        }
    }
    return i;
}

static inline void new_report(WINE_HID_REPORT *wine_report, struct feature* feature)
{
    wine_report->reportID = feature->caps.ReportID;
    wine_report->dwSize = sizeof(*wine_report) - sizeof(WINE_HID_ELEMENT);
    wine_report->elementCount = 0;
}

static void build_elements(WINE_HID_REPORT *wine_report, struct feature* feature, DWORD *bitOffset)
{
    unsigned int i;

    if (!feature->isData)
    {
        *bitOffset = *bitOffset + (feature->caps.BitSize * feature->caps.ReportCount);
        return;
    }

    for (i = 0; i < feature->caps.usage_count; i++)
    {
        WINE_HID_ELEMENT *wine_element = &wine_report->Elements[wine_report->elementCount];

        wine_element->valueStartBit = *bitOffset;
        if (feature->caps.UsagePage == HID_USAGE_PAGE_BUTTON)
        {
            wine_element->ElementType = ButtonElement;
            wine_element->caps.button.UsagePage = feature->caps.UsagePage;
            wine_element->caps.button.ReportID = feature->caps.ReportID;
            wine_element->caps.button.BitField = feature->BitField;
            wine_element->caps.button.IsRange = feature->caps.IsRange;
            wine_element->caps.button.IsStringRange = feature->caps.IsStringRange;
            wine_element->caps.button.IsDesignatorRange = feature->caps.IsDesignatorRange;
            wine_element->caps.button.IsAbsolute = feature->IsAbsolute;
            if (wine_element->caps.button.IsRange)
            {
                wine_element->bitCount = (feature->caps.u.Range.UsageMax - feature->caps.u.Range.UsageMin) + 1;
                *bitOffset = *bitOffset + wine_element->bitCount;
                wine_element->caps.button.u.Range.UsageMin = feature->caps.u.Range.UsageMin;
                wine_element->caps.button.u.Range.UsageMax = feature->caps.u.Range.UsageMax;
                wine_element->caps.button.u.Range.StringMin = feature->caps.u.Range.StringMin;
                wine_element->caps.button.u.Range.StringMax = feature->caps.u.Range.StringMax;
                wine_element->caps.button.u.Range.DesignatorMin = feature->caps.u.Range.DesignatorMin;
                wine_element->caps.button.u.Range.DesignatorMax = feature->caps.u.Range.DesignatorMax;
            }
            else
            {
                *bitOffset = *bitOffset + 1;
                wine_element->bitCount = 1;
                wine_element->caps.button.u.NotRange.Usage = feature->caps.u.NotRange.Usage[i];
                wine_element->caps.button.u.NotRange.StringIndex = feature->caps.u.NotRange.StringIndex;
                wine_element->caps.button.u.NotRange.DesignatorIndex = feature->caps.u.NotRange.DesignatorIndex;
            }
        }
        else
        {
            wine_element->ElementType = ValueElement;
            wine_element->caps.value.UsagePage = feature->caps.UsagePage;
            wine_element->caps.value.ReportID = feature->caps.ReportID;
            wine_element->caps.value.BitField = feature->BitField;
            wine_element->caps.value.IsRange = feature->caps.IsRange;
            wine_element->caps.value.IsStringRange = feature->caps.IsStringRange;
            wine_element->caps.value.IsDesignatorRange = feature->caps.IsDesignatorRange;
            wine_element->caps.value.IsAbsolute = feature->IsAbsolute;
            wine_element->caps.value.HasNull = feature->HasNull;
            wine_element->caps.value.BitSize = feature->caps.BitSize;
            if (feature->caps.usage_count > 1)
            {
                if (feature->caps.ReportCount > feature->caps.usage_count)
                    wine_element->caps.value.ReportCount = feature->caps.ReportCount / feature->caps.usage_count;
                else
                    wine_element->caps.value.ReportCount = 1;
            }
            else
                wine_element->caps.value.ReportCount = feature->caps.ReportCount;
            wine_element->bitCount = (feature->caps.BitSize * wine_element->caps.value.ReportCount);
            *bitOffset = *bitOffset + wine_element->bitCount;
            wine_element->caps.value.UnitsExp = feature->caps.UnitsExp;
            wine_element->caps.value.Units = feature->caps.Units;
            wine_element->caps.value.LogicalMin = feature->caps.LogicalMin;
            wine_element->caps.value.LogicalMax = feature->caps.LogicalMax;
            wine_element->caps.value.PhysicalMin = feature->caps.PhysicalMin;
            wine_element->caps.value.PhysicalMax = feature->caps.PhysicalMax;
            if (wine_element->caps.value.IsRange)
            {
                wine_element->caps.value.u.Range.UsageMin = feature->caps.u.Range.UsageMin;
                wine_element->caps.value.u.Range.UsageMax = feature->caps.u.Range.UsageMax;
                wine_element->caps.value.u.Range.StringMin = feature->caps.u.Range.StringMin;
                wine_element->caps.value.u.Range.StringMax = feature->caps.u.Range.StringMax;
                wine_element->caps.value.u.Range.DesignatorMin = feature->caps.u.Range.DesignatorMin;
                wine_element->caps.value.u.Range.DesignatorMax = feature->caps.u.Range.DesignatorMax;
            }
            else
            {
                wine_element->caps.value.u.NotRange.Usage = feature->caps.u.NotRange.Usage[i];
                wine_element->caps.value.u.NotRange.StringIndex = feature->caps.u.NotRange.StringIndex;
                wine_element->caps.value.u.NotRange.DesignatorIndex = feature->caps.u.NotRange.DesignatorIndex;
            }
        }

        wine_report->elementCount++;
    }
}

static void count_elements(struct feature* feature, USHORT *buttons, USHORT *values)
{
    if (feature->caps.UsagePage == HID_USAGE_PAGE_BUTTON)
    {
        if (feature->caps.IsRange)
            *buttons = *buttons + 1;
        else
            *buttons = *buttons + feature->caps.usage_count;
    }
    else
    {
        if (feature->caps.IsRange)
            *values = *values + 1;
        else
            *values = *values + feature->caps.usage_count;
    }
}

static WINE_HIDP_PREPARSED_DATA* build_PreparseData(
                       struct feature **features, int feature_count,
                       struct feature **input_features, int i_count,
                       struct feature **output_features, int o_count,
                       struct feature **feature_features, int f_count,
                       struct collection *base_collection)
{
    WINE_HIDP_PREPARSED_DATA *data;
    WINE_HID_REPORT *wine_report = NULL;
    DWORD bitOffset, bitLength;
    unsigned int report_count = 1;
    unsigned int i;
    unsigned int element_count;
    unsigned int size = 0;

    if (features[0]->caps.ReportID != 0)
    {
        unsigned int *report_ids;
        unsigned int cnt = max(i_count, o_count);
        cnt = max(cnt, f_count);
        report_ids = HeapAlloc(GetProcessHeap(), 0 , sizeof(*report_ids) * cnt);

        if (i_count)
        {
            report_ids[0] = input_features[0]->caps.ReportID;
            for (i = 1; i < i_count; i++)
            {
                unsigned int j;
                unsigned int found = FALSE;
                for (j = 0; !found && j < i_count; j++)
                {
                    if (report_ids[j] == input_features[i]->caps.ReportID)
                        found = TRUE;
                }
                if (!found)
                {
                    report_ids[report_count] = input_features[i]->caps.ReportID;
                    report_count++;
                }
            }
        }
        if (o_count)
        {
            report_count++;
            report_ids[0] = output_features[0]->caps.ReportID;
            for (i = 1; i < o_count; i++)
            {
                unsigned int j;
                unsigned int found = FALSE;
                for (j = 0; !found && j < o_count; j++)
                {
                    if (report_ids[j] == output_features[i]->caps.ReportID)
                        found = TRUE;
                }
                if (!found)
                {
                    report_ids[report_count] = output_features[i]->caps.ReportID;
                    report_count++;
                }
            }
        }
        if (f_count)
        {
            report_count++;
            report_ids[0] = feature_features[0]->caps.ReportID;
            for (i = 1; i < f_count; i++)
            {
                unsigned int j;
                unsigned int found = FALSE;
                for (j = 0; !found && j < f_count; j++)
                {
                    if (report_ids[j] == feature_features[i]->caps.ReportID)
                        found = TRUE;
                }
                if (!found)
                {
                    report_ids[report_count] = feature_features[i]->caps.ReportID;
                    report_count++;
                }
            }
        }
        HeapFree(GetProcessHeap(), 0, report_ids);
    }
    else
    {
        if (o_count) report_count++;
        if (f_count) report_count++;
    }

    element_count = 0;
    for (i = 0; i < feature_count; i++)
        element_count += features[i]->caps.usage_count;

    size = sizeof(WINE_HIDP_PREPARSED_DATA) +
            (element_count * sizeof(WINE_HID_ELEMENT)) +
            (report_count * sizeof(WINE_HID_REPORT));

    TRACE("%i reports %i elements -> size %i\n",report_count, element_count, size);

    data = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
    data->magic = HID_MAGIC;
    data->dwSize = size;
    data->caps.Usage = base_collection->caps.u.NotRange.Usage[0];
    data->caps.UsagePage = base_collection->caps.UsagePage;

    wine_report = data->InputReports;
    if (i_count)
    {
        bitLength = 0;
        new_report(wine_report, input_features[0]);
        data->dwInputReportCount++;

        /* Room for the reportID */
        bitOffset = 8;

        for (i = 0; i < i_count; i++)
        {
            if (input_features[i]->caps.ReportID != wine_report->reportID)
            {
                wine_report->dwSize += (sizeof(WINE_HID_ELEMENT) * wine_report->elementCount);
                wine_report = (WINE_HID_REPORT*)(((BYTE*)wine_report)+wine_report->dwSize);
                new_report(wine_report, input_features[i]);
                data->dwInputReportCount++;
                bitLength = max(bitOffset, bitLength);
                bitOffset = 8;
            }
            build_elements(wine_report, input_features[i], &bitOffset);
            count_elements(input_features[i], &data->caps.NumberInputButtonCaps,
                &data->caps.NumberInputValueCaps);
        }
        wine_report->dwSize += (sizeof(WINE_HID_ELEMENT) * wine_report->elementCount);
        bitLength = max(bitOffset, bitLength);
        data->caps.InputReportByteLength = ((bitLength + 7) & ~7)/8;
    }

    if (o_count)
    {
        bitLength = 0;
        wine_report = (WINE_HID_REPORT*)(((BYTE*)wine_report)+wine_report->dwSize);
        data->dwOutputReportOffset = (BYTE*)wine_report - (BYTE*)data->InputReports;
        new_report(wine_report, output_features[0]);
        data->dwOutputReportCount++;
        bitOffset = 8;

        for (i = 0; i < o_count; i++)
        {
            if (output_features[i]->caps.ReportID != wine_report->reportID)
            {
                wine_report->dwSize += (sizeof(WINE_HID_ELEMENT) * wine_report->elementCount);
                wine_report = (WINE_HID_REPORT*)(((BYTE*)wine_report)+wine_report->dwSize);
                new_report(wine_report, output_features[i]);
                data->dwOutputReportCount++;
                bitLength = max(bitOffset, bitLength);
                bitOffset = 8;
            }
            build_elements(wine_report, output_features[i], &bitOffset);
            count_elements(output_features[i], &data->caps.NumberOutputButtonCaps,
                &data->caps.NumberOutputValueCaps);
        }
        wine_report->dwSize += (sizeof(WINE_HID_ELEMENT) * wine_report->elementCount);
        bitLength = max(bitOffset, bitLength);
        data->caps.OutputReportByteLength = ((bitLength + 7) & ~7)/8;
    }

    if (f_count)
    {
        bitLength = 0;
        wine_report = (WINE_HID_REPORT*)(((BYTE*)wine_report)+wine_report->dwSize);
        data->dwFeatureReportOffset = (BYTE*)wine_report - (BYTE*)data->InputReports;
        new_report(wine_report, feature_features[0]);
        data->dwFeatureReportCount++;
        bitOffset = 8;

        for (i = 0; i < f_count; i++)
        {
            if (feature_features[i]->caps.ReportID != wine_report->reportID)
            {
                wine_report->dwSize += (sizeof(WINE_HID_ELEMENT) * wine_report->elementCount);
                wine_report = (WINE_HID_REPORT*)((BYTE*)wine_report+wine_report->dwSize);
                new_report(wine_report, feature_features[i]);
                data->dwFeatureReportCount++;
                bitLength = max(bitOffset, bitLength);
                bitOffset = 8;
            }
            build_elements(wine_report, feature_features[i], &bitOffset);
            count_elements(feature_features[i], &data->caps.NumberFeatureButtonCaps,
                &data->caps.NumberFeatureValueCaps);
        }
        bitLength = max(bitOffset, bitLength);
        data->caps.FeatureReportByteLength = ((bitLength + 7) & ~7)/8;
    }

    return data;
}

static void free_collection(struct collection *collection)
{
    struct feature *fentry, *fnext;
    struct collection *centry, *cnext;
    LIST_FOR_EACH_ENTRY_SAFE(centry, cnext, &collection->collections, struct collection, entry)
    {
        list_remove(&centry->entry);
        free_collection(centry);
    }
    LIST_FOR_EACH_ENTRY_SAFE(fentry, fnext, &collection->features, struct feature, col_entry)
    {
        list_remove(&fentry->col_entry);
        HeapFree(GetProcessHeap(), 0, fentry);
    }
    HeapFree(GetProcessHeap(), 0, collection);
}

static int compare_reports(const void *a, const void* b)
{
    struct feature *f1 = *(struct feature **)a;
    struct feature *f2 = *(struct feature **)b;
    int c = (f1->caps.ReportID - f2->caps.ReportID);
    if (c) return c;
    return (f1->index - f2->index);
}

WINE_HIDP_PREPARSED_DATA* ParseDescriptor(BYTE *descriptor, unsigned int length)
{
    WINE_HIDP_PREPARSED_DATA *data = NULL;
    struct collection *base;
    struct caps caps;

    struct list features;
    struct list caps_stack;

    unsigned int feature_count = 0;
    unsigned int cidx;

    if (TRACE_ON(hid))
    {
        TRACE("Descriptor[%i]: ", length);
        for (cidx = 0; cidx < length; cidx++)
        {
            TRACE("%x ",descriptor[cidx]);
            if ((cidx+1) % 80 == 0)
                TRACE("\n");
        }
        TRACE("\n");
    }

    list_init(&features);
    list_init(&caps_stack);

    base = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*base));
    base->index = 1;
    list_init(&base->features);
    list_init(&base->collections);
    memset(&caps, 0, sizeof(caps));

    cidx = 0;
    parse_descriptor(descriptor, 0, length, &feature_count, &cidx, base, &caps, &features, &caps_stack);

    debug_collection(base);

    if (!list_empty(&caps_stack))
    {
        struct caps_stack *entry, *cursor;
        ERR("%i unpopped device caps on the stack\n", list_count(&caps_stack));
        LIST_FOR_EACH_ENTRY_SAFE(entry, cursor, &caps_stack, struct caps_stack, entry)
        {
            list_remove(&entry->entry);
            HeapFree(GetProcessHeap(), 0, entry);
        }
    }

    cidx = 2;
    if (feature_count)
    {
        struct feature *entry;
        struct feature** sorted_features;
        struct feature** input_features;
        struct feature** output_features;
        struct feature** feature_features;
        unsigned int i_count, o_count, f_count;
        unsigned int i;

        i_count = o_count = f_count = 0;

        sorted_features = HeapAlloc(GetProcessHeap(), 0, sizeof(*sorted_features) * feature_count);
        input_features = HeapAlloc(GetProcessHeap(), 0, sizeof(*input_features) * feature_count);
        output_features = HeapAlloc(GetProcessHeap(), 0, sizeof(*output_features) * feature_count);
        feature_features = HeapAlloc(GetProcessHeap(), 0, sizeof(*feature_features) * feature_count);

        i = 0;
        LIST_FOR_EACH_ENTRY(entry, &features, struct feature, entry)
            sorted_features[i++] = entry;

        /* Sort features base on report if there are multiple reports */
        if (sorted_features[0]->caps.ReportID != 0)
            qsort(sorted_features, feature_count, sizeof(struct feature*), compare_reports);

        for (i = 0; i < feature_count; i++)
        {
            switch (sorted_features[i]->type)
            {
                case HidP_Input:
                    input_features[i_count] = sorted_features[i];
                    i_count++;
                    break;
                case HidP_Output:
                    output_features[o_count] = sorted_features[i];
                    o_count++;
                    break;
                case HidP_Feature:
                    feature_features[f_count] = sorted_features[i];
                    f_count++;
                    break;
                default:
                    ERR("Unknown type %i\n",sorted_features[i]->type);
            }
        }

        if (TRACE_ON(hid))
        {
            TRACE("DUMP FEATURES:\n");
            TRACE("----INPUT----\n");
            for (cidx = 0; cidx < i_count; cidx++)
                debug_feature(input_features[cidx]);
            TRACE("----OUTPUT----\n");
            for (cidx = 0; cidx < o_count; cidx++)
                debug_feature(output_features[cidx]);
            TRACE("----FEATURE----\n");
            for (cidx = 0; cidx < f_count; cidx++)
                debug_feature(feature_features[cidx]);
        }

        data = build_PreparseData(sorted_features, feature_count, input_features, i_count, output_features, o_count, feature_features, f_count, base);

        debug_print_preparsed(data);

        HeapFree(GetProcessHeap(), 0, sorted_features);
        HeapFree(GetProcessHeap(), 0, input_features);
        HeapFree(GetProcessHeap(), 0, output_features);
        HeapFree(GetProcessHeap(), 0, feature_features);
    }

    free_collection(base);
    /* We do not have to free the list as free_collection does all the work */

    return data;
}
