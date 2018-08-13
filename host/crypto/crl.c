// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "crl.h"
#include <openenclave/internal/crl.h>
#include <openenclave/internal/enclavelibc.h>
#include <openenclave/internal/print.h>
#include <openenclave/internal/raise.h>
#include <openssl/x509.h>
#include <openssl/asn1.h>
#include <string.h>
#include <time.h>
#include <limits.h>

/* Randomly generated magic number */
#define OE_CRL_MAGIC 0xe8c993b1cca24906

OE_STATIC_ASSERT(sizeof(crl_t) <= sizeof(oe_crl_t));

OE_INLINE void _crl_init(crl_t* impl, X509_CRL* crl)
{
    impl->magic = OE_CRL_MAGIC;
    impl->crl = crl;
}

bool crl_is_valid(const crl_t* impl)
{
    return impl && (impl->magic == OE_CRL_MAGIC) && impl->crl;
}

OE_INLINE void _crl_free(crl_t* impl)
{
    X509_CRL_free(impl->crl);
    memset(impl, 0, sizeof(crl_t));
}

oe_result_t oe_crl_read_der(
    oe_crl_t* crl,
    const uint8_t* der_data,
    size_t der_size)
{
    oe_result_t result = OE_UNEXPECTED;
    crl_t* impl = (crl_t*)crl;
    BIO* bio = NULL;
    X509_CRL* x509_crl = NULL;

    /* Clear the implementation */
    if (impl)
        memset(impl, 0, sizeof(crl_t));

    /* Check for invalid parameters */
    if (!der_data || !der_size || !crl)
        OE_RAISE(OE_UNEXPECTED);

    /* Create a BIO for reading the DER-formatted data */
    if (!(bio = BIO_new_mem_buf(der_data, der_size)))
        goto done;

    /* Read BIO into X509_CRL object */
    if (!(x509_crl = d2i_X509_CRL_bio(bio, NULL)))
        goto done;

    /* Initialize the implementation */
    _crl_init(impl, x509_crl);
    x509_crl = NULL;

    result = OE_OK;

done:

    if (x509_crl)
        X509_CRL_free(x509_crl);

    if (bio)
        BIO_free(bio);

    return result;
}

oe_result_t oe_crl_free(oe_crl_t* crl)
{
    oe_result_t result = OE_UNEXPECTED;
    crl_t* impl = (crl_t*)crl;

    /* Check the parameter */
    if (!crl_is_valid(impl))
        OE_RAISE(OE_INVALID_PARAMETER);

    /* Free the CRL */
    _crl_free(impl);

    result = OE_OK;

done:
    return result;
}

/* Parse a string into a oe_date_t: example: "May 30 10:23:42 2018 GMT" */
static oe_result_t _string_to_date(const char* str, oe_date_t* date)
{
    oe_result_t result = OE_UNEXPECTED;
    static const char* _month[] = 
    {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };

    const char* p = str;

    if (date)
        memset(date, 0, sizeof(oe_date_t));

    /* Parse the month */
    {
        date->month = UINT_MAX;

        for (uint32_t i = 0; i < OE_COUNTOF(_month); i++)
        {
            if (strncmp(p, _month[i], 3) == 0)
            {
                date->month = i + 1;
                p += 3;
            }
        }

        if (date->month == UINT_MAX || *p++ != ' ')
            OE_RAISE(OE_FAILURE);
    }

    /* Parse the day of the month */
    {
        char* end;
        unsigned long day = strtoul(p, &end, 10);

        if (p == end || *end != ' ' || day < 1 || day > 31)
            OE_RAISE(OE_FAILURE);

        date->day = (uint32_t)day;
        p = end + 1;
    }

    /* Parse the hours */
    {
        char* end;
        unsigned long hours = strtoul(p, &end, 10);

        if (p == end || *end != ':' || hours > 23)
            OE_RAISE(OE_FAILURE);

        date->hours = (uint32_t)hours;
        p = end + 1;
    }

    /* Parse the minutes */
    {
        char* end;
        unsigned long minutes = strtoul(p, &end, 10);

        if (p == end || *end != ':' || minutes > 59)
            OE_RAISE(OE_FAILURE);

        date->minutes = (uint32_t)minutes;
        p = end + 1;
    }

    /* Parse the seconds */
    {
        char* end;
        unsigned long seconds = strtoul(p, &end, 10);

        if (p == end || *end != ' ' || seconds > 59)
            OE_RAISE(OE_FAILURE);

        date->seconds = (uint32_t)seconds;
        p = end + 1;
    }

    /* Parse the year */
    {
        char* end;
        unsigned long year = strtoul(p, &end, 10);

        if (p == end || *end != ' ')
            OE_RAISE(OE_FAILURE);

        date->year = (uint32_t)year;
        p = end + 1;
    }

    /* Check for "GMT" string at the end */
    if (strcmp(p, "GMT") != 0)
        OE_RAISE(OE_FAILURE);

    result = OE_OK;

done:
    return result;
}

static oe_result_t _asn1_time_to_date(const ASN1_TIME* time, oe_date_t* date)
{
    oe_result_t result = OE_UNEXPECTED;
    struct tm;
    BIO* bio = NULL;
    BUF_MEM* mem;
    const char null_terminator = '\0';

    if (!(bio = BIO_new(BIO_s_mem())))
        OE_RAISE(OE_FAILURE);

    if (!ASN1_TIME_print(bio, time))
        OE_RAISE(OE_FAILURE);

    if (!BIO_get_mem_ptr(bio, &mem))
        OE_RAISE(OE_FAILURE);

    if (BIO_write(bio, &null_terminator, sizeof(null_terminator)) <= 0)
        OE_RAISE(OE_FAILURE);

    OE_CHECK(_string_to_date(mem->data, date));

    result = OE_OK;

done:

    if (bio)
        BIO_free(bio);

    return result;
}

oe_result_t oe_crl_get_update_dates(
    const oe_crl_t* crl,
    oe_date_t* last,
    oe_date_t* next)
{
    oe_result_t result = OE_UNEXPECTED;
    const crl_t* impl = (const crl_t*)crl;

    if (last)
        memset(last, 0, sizeof(oe_date_t));

    if (next)
        memset(next, 0, sizeof(oe_date_t));

    if (!crl_is_valid(impl))
        OE_RAISE(OE_INVALID_PARAMETER);

    if (last)
    {
        ASN1_TIME* time;

        if (!(time = X509_CRL_get_lastUpdate(impl->crl)))
            OE_RAISE(OE_FAILURE);

        OE_CHECK(_asn1_time_to_date(time, last));
    }

    if (next)
    {
        ASN1_TIME* time;

        if (!(time = X509_CRL_get_nextUpdate(impl->crl)))
            OE_RAISE(OE_FAILURE);

        OE_CHECK(_asn1_time_to_date(time, next));
    }

    result = OE_OK;

done:

    return result;
}
