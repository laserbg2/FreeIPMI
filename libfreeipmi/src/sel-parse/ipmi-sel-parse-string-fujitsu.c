/*
  Copyright (C) 2003-2010 FreeIPMI Core Team

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2, or (at your option)
  any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software Foundation,
  Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <stdlib.h>
#if STDC_HEADERS
#include <string.h>
#endif /* STDC_HEADERS */
#include <assert.h>
#include <errno.h>

#include "freeipmi/sel-parse/ipmi-sel-parse.h"

#include "freeipmi/cmds/ipmi-device-global-cmds.h"
#include "freeipmi/cmds/ipmi-sel-cmds.h"
#include "freeipmi/spec/ipmi-netfn-spec.h"
#include "freeipmi/spec/ipmi-comp-code-spec.h"
#include "freeipmi/record-format/ipmi-sdr-record-format.h"
#include "freeipmi/record-format/ipmi-sel-record-format.h"
#include "freeipmi/spec/ipmi-cmd-oem-spec.h"
#include "freeipmi/spec/ipmi-event-reading-type-code-spec.h"
#include "freeipmi/spec/ipmi-event-reading-type-code-oem-spec.h"
#include "freeipmi/spec/ipmi-iana-enterprise-numbers-spec.h"
#include "freeipmi/spec/ipmi-product-id-spec.h"
#include "freeipmi/spec/ipmi-sensor-and-event-code-tables-spec.h"
#include "freeipmi/spec/ipmi-sensor-and-event-code-tables-oem-spec.h"
#include "freeipmi/spec/ipmi-sensor-numbers-oem-spec.h"
#include "freeipmi/spec/ipmi-sensor-types-spec.h"
#include "freeipmi/spec/ipmi-sensor-types-oem-spec.h"
#include "freeipmi/spec/ipmi-slave-address-spec.h"
#include "freeipmi/spec/ipmi-slave-address-oem-spec.h"
#include "freeipmi/util/ipmi-iana-enterprise-numbers-util.h"
#include "freeipmi/util/ipmi-sensor-and-event-code-tables-util.h"

#include "ipmi-sel-parse-common.h"
#include "ipmi-sel-parse-defs.h"
#include "ipmi-sel-parse-string.h"
#include "ipmi-sel-parse-string-dell.h"
#include "ipmi-sel-parse-trace.h"
#include "ipmi-sel-parse-util.h"

#include "freeipmi-portability.h"

/* XXX: achu - duplicated from ipmi-oem.c, no where else appropriate to stick it??? */

/* All required cmd definitions are in ipmi-cmd-oem-spec.h */

#define IPMI_OEM_FUJITSU_COMMAND_SPECIFIER_GET_SEL_ENTRY_LONG_TEXT      0x43
#define IPMI_OEM_FUJITSU_COMMAND_SPECIFIER_GET_SEL_ENTRY_TEXT           0x45

/* Fully decoded english version of decoded SEL */
#define IPMI_OEM_FUJITSU_SEL_ENTRY_LONG_TEXT_MAX_STRING_LENGTH          100

/* (very) Short text to display on 1 line of the optional LCD Panel, 20 characters */
#define IPMI_OEM_FUJITSU_SEL_ENTRY_TEXT_MAX_STRING_LENGTH                20

/*
 * CSS (Customer Self Service)
 * If the component is marked as CSS, the customer can replace it by himself
 * without a service technican (e.g. Memory DIMM etc.)
 * CSS is combined with the severity information.
 */
#define IPMI_OEM_FUJITSU_CSS_BITMASK            0x80
#define IPMI_OEM_FUJITSU_CSS_SHIFT              7

#define IPMI_OEM_FUJITSU_CSS_COMPONENT          1
#define IPMI_OEM_FUJITSU_NO_CSS_COMPONENT       0

/*
 * Severity of a decoded event. All events should have an assigned severity.
 */
#define IPMI_OEM_FUJITSU_SEVERITY_BITMASK       0x70
#define IPMI_OEM_FUJITSU_SEVERITY_SHIFT         4

#define IPMI_OEM_FUJITSU_SEVERITY_INFORMATIONAL 0
#define IPMI_OEM_FUJITSU_SEVERITY_MINOR         1
#define IPMI_OEM_FUJITSU_SEVERITY_MAJOR         2
#define IPMI_OEM_FUJITSU_SEVERITY_CRITICAL      3

#define IPMI_OEM_FUJITSU_MAX_BYTES 256

/*
 * HLiebig: This is a stripped down version of ipmi_oem_fujitsu_get_sel_entry_long_text() 
 * in ipmi-oem/src/ipmi-oem-fujitsu.c
 * tested against 
 * TX200S3 (iRMC S1)
 * TX120S2/TX300S6 (iRMC S2)
 */
static int
_ipmi_sel_oem_fujitsu_get_sel_entry_long_text (ipmi_sel_parse_ctx_t ctx,
                                               struct ipmi_sel_parse_entry *sel_parse_entry,
                                               char *buf,
                                               unsigned int buflen,
                                               int include_severity)
{
  uint8_t bytes_rq[IPMI_OEM_FUJITSU_MAX_BYTES];
  uint8_t bytes_rs[IPMI_OEM_FUJITSU_MAX_BYTES];
  int rs_len;
  uint16_t sel_record_id = 0xFFFF;
  uint8_t css = 0;
  uint8_t severity = 0;
  char string_buf[IPMI_OEM_FUJITSU_SEL_ENTRY_LONG_TEXT_MAX_STRING_LENGTH + 1];
  uint8_t data_length;
  uint8_t max_read_length;
  uint8_t offset = 0;
  uint8_t component_length = 0;
  const char *css_str = NULL;
  const char *severity_str = NULL;
  int rv = -1;

  assert (ctx);
  assert (ctx->magic == IPMI_SEL_PARSE_CTX_MAGIC);
  assert (ctx->manufacturer_id == IPMI_IANA_ENTERPRISE_ID_FUJITSU);
  assert (sel_parse_entry);
  assert (buf);
  assert (buflen);

  /* Get current SEL record ID we are working on */
  if (sel_parse_get_record_header_info (ctx,
                                        sel_parse_entry,
                                        &sel_record_id,
                                        NULL) < 0)
    goto cleanup;

  /* Holger: XXX: why this exception?? */
  if (sel_record_id == IPMI_SEL_GET_RECORD_ID_LAST_ENTRY)
    goto cleanup;

  memset (string_buf, '\0', IPMI_OEM_FUJITSU_SEL_ENTRY_LONG_TEXT_MAX_STRING_LENGTH + 1);

  /* Note: Referenced documentation is for iRMC S2 version */
  if (IPMI_FUJITSU_PRODUCT_ID_IS_IRMC_S1 (ctx->product_id))
    {
      /* iRMC S1 has limits */
      max_read_length = 32;
      data_length = 80;
    }
  else 
    {
      max_read_length = IPMI_OEM_FUJITSU_SEL_ENTRY_LONG_TEXT_MAX_STRING_LENGTH;
      data_length = IPMI_OEM_FUJITSU_SEL_ENTRY_LONG_TEXT_MAX_STRING_LENGTH;
    }

  /* Fujitsu OEM Command
   * 
   * http://manuals.ts.fujitsu.com/file/4390/irmc_s2-ug-en.pdf
   *
   * Request
   *
   * 0x2E - OEM network function
   * 0xF5 - OEM cmd
   * 0x80 - Fujitsu IANA (LSB first)
   * 0x28 - Fujitsu IANA
   * 0x00 - Fujitsu IANA
   * 0x43 - Command Specifier
   * 0x?? - Record ID (LSB first)
   * 0x?? - Record ID ; 0x0000 = "first record", 0xFFFF = "last record"
   * 0x?? - Offset (in response SEL text)
   * 0x?? - MaxResponseDataSize (size of converted SEL data 16:n in response, maximum is 100)
   *
   * Response
   *
   * 0xF5 - OEM cmd
   * 0x?? - Completion code
   * 0x80 - Fujitsu IANA (LSB first)
   * 0x28 - Fujitsu IANA
   * 0x00 - Fujitsu IANA
   * 0x?? - Next Record ID (LSB)
   * 0x?? - Next Record ID (MSB)
   * 0x?? - Actual Record ID (LSB)
   * 0x?? - Actual Record ID (MSB)
   * 0x?? - Record type
   * 0x?? - timestamp (LSB first)
   * 0x?? - timestamp
   * 0x?? - timestamp
   * 0x?? - timestamp
   * 0x?? - severity   
   *      bit 7   - CSS component
   *              - 0 - No CSS component
   *              - 1 - CSS component
   *      bit 6-4 - 000 = INFORMATIONAL
   *                001 = MINOR
   *                010 = MAJOR
   *                011 = CRITICAL
   *                1xx = unknown
   *      bit 3-0 - reserved
   * 0x?? - data length (of the whole text)
   * 0x?? - converted SEL data
   *      - requested number of bytes starting at requested offset (MaxResponseDataSize-1 bytes of data)
   * 0x00 - trailing '\0' character
   */
     
  bytes_rq[0] = IPMI_CMD_OEM_FUJITSU_SYSTEM;
  bytes_rq[1] = (IPMI_IANA_ENTERPRISE_ID_FUJITSU & 0x0000FF);
  bytes_rq[2] = (IPMI_IANA_ENTERPRISE_ID_FUJITSU & 0x00FF00) >> 8;
  bytes_rq[3] = (IPMI_IANA_ENTERPRISE_ID_FUJITSU & 0xFF0000) >> 16;
  bytes_rq[4] = IPMI_OEM_FUJITSU_COMMAND_SPECIFIER_GET_SEL_ENTRY_LONG_TEXT;
  bytes_rq[5] = (sel_record_id & 0x00FF);
  bytes_rq[6] = (sel_record_id & 0xFF00) >> 8;
  /* Request partial or complete string, depending on product */
  bytes_rq[8] = max_read_length;

  while (offset < data_length)
    {
      bytes_rq[7] = offset;

      /* BMC checks for boundaries, offset + len has to be <= 80 (iRMC S1) <= 100 (iRMC S2) */ 
      if (offset + bytes_rq[8] > data_length)
         bytes_rq[8] = data_length - offset;

      if ((rs_len = ipmi_cmd_raw (ctx->ipmi_ctx,
                                  0, /* lun */
                                  IPMI_NET_FN_OEM_GROUP_RQ, /* network function */
                                  bytes_rq, /* data */
                                  9, /* num bytes */
                                  bytes_rs,
                                  IPMI_OEM_FUJITSU_MAX_BYTES)) < 0)
        {
          SEL_PARSE_SET_ERRNUM (ctx, IPMI_SEL_PARSE_ERR_IPMI_ERROR);
          goto cleanup;
        }
      
      if (rs_len < 17)
        {
          if (rs_len >= 2 && bytes_rs[1] != IPMI_COMP_CODE_COMMAND_SUCCESS)
            {
              if (bytes_rs[1] == IPMI_COMP_CODE_INSUFFICIENT_PRIVILEGE_LEVEL)
                {
                  snprintf (string_buf,
                            IPMI_OEM_FUJITSU_SEL_ENTRY_LONG_TEXT_MAX_STRING_LENGTH,
                            "[Fujtisu OEM decoding requires administrator privilege]");
                  goto out;
                }
              goto cleanup;
            }
        }
      
      /* Get severity and CSS flag only once */
      if (!offset)
        {
          css = (bytes_rs[14] & IPMI_OEM_FUJITSU_CSS_BITMASK);
          css >>= IPMI_OEM_FUJITSU_CSS_SHIFT;
          
          severity = (bytes_rs[14] & IPMI_OEM_FUJITSU_SEVERITY_BITMASK);
          severity >>= IPMI_OEM_FUJITSU_SEVERITY_SHIFT;
        }
      
      data_length = bytes_rs[15];
      
      bytes_rs[rs_len-1] = '\0'; /* just to be sure it's terminated */
      component_length = strlen ((char *)bytes_rs + 16);
      
      /* achu: truncate if there is overflow */
      if (offset + component_length > IPMI_OEM_FUJITSU_SEL_ENTRY_LONG_TEXT_MAX_STRING_LENGTH)
        {
          memcpy (string_buf + offset,
                  &bytes_rs[16],
                  IPMI_OEM_FUJITSU_SEL_ENTRY_LONG_TEXT_MAX_STRING_LENGTH - offset);
          offset = data_length;
        }
      else
        {
          memcpy (string_buf + offset,
                  &bytes_rs[16],
                  component_length);
          offset += component_length;
        }
    }
  
  if (css == IPMI_OEM_FUJITSU_CSS_COMPONENT)
    css_str = "CSS Component";
  
  if (include_severity)
    {
      if (severity == IPMI_OEM_FUJITSU_SEVERITY_INFORMATIONAL)
        severity_str = "INFORMATIONAL";
      else if (severity == IPMI_OEM_FUJITSU_SEVERITY_MINOR)
        severity_str = "MINOR";
      else if (severity == IPMI_OEM_FUJITSU_SEVERITY_MAJOR)
        severity_str = "MAJOR";
      else if (severity == IPMI_OEM_FUJITSU_SEVERITY_CRITICAL)
        severity_str = "CRITICAL";
      else
        severity_str = "Unknown Severity";
      
      if (css_str)
        snprintf (string_buf,
                  IPMI_OEM_FUJITSU_SEL_ENTRY_LONG_TEXT_MAX_STRING_LENGTH,
                  "%s: %s (%s)",
                  severity_str,
                  string_buf,
                  css_str);
      else 
        snprintf (string_buf,
                  IPMI_OEM_FUJITSU_SEL_ENTRY_LONG_TEXT_MAX_STRING_LENGTH,
                  "%s: %s",
                  severity_str,
                  string_buf);
    }
  else
    {
      if (css_str)
        snprintf (string_buf,
                  IPMI_OEM_FUJITSU_SEL_ENTRY_LONG_TEXT_MAX_STRING_LENGTH,
                  "%s (%s)",
                  string_buf,
                  css_str);
      else
        snprintf (string_buf,
                  IPMI_OEM_FUJITSU_SEL_ENTRY_LONG_TEXT_MAX_STRING_LENGTH,
                  "%s",
                  string_buf);
    }
  
 out:

  if (strlen (string_buf) > buflen)
    {
      SEL_PARSE_SET_ERRNUM (ctx, IPMI_SEL_PARSE_ERR_INTERNAL_ERROR);
      goto cleanup;
    }
  
  memcpy (buf, string_buf, strlen (string_buf));
  
  rv = 0;
 cleanup:
  return (rv);
}

/* return (0) - no OEM match
 * return (1) - OEM match
 * return (-1) - error, cleanup and return error
 *
 * 0 - continue on
 * 1 - buffer full, return full buffer to user
 */
int
ipmi_sel_parse_output_fujitsu_event_data1_class_sensor_specific_discrete (ipmi_sel_parse_ctx_t ctx,
                                                                          struct ipmi_sel_parse_entry *sel_parse_entry,
                                                                          uint8_t sel_record_type,
                                                                          char *tmpbuf,
                                                                          unsigned int tmpbuflen,
                                                                          unsigned int flags,
                                                                          unsigned int *wlen,
                                                                          struct ipmi_sel_system_event_record_data *system_event_record_data)

{
  assert (ctx);
  assert (ctx->magic == IPMI_SEL_PARSE_CTX_MAGIC);
  assert (ctx->manufacturer_id == IPMI_IANA_ENTERPRISE_ID_FUJITSU);
  assert (sel_parse_entry);
  assert (tmpbuf);
  assert (tmpbuflen);
  assert (!(flags & ~IPMI_SEL_PARSE_STRING_MASK));
  assert (flags & IPMI_SEL_PARSE_STRING_FLAGS_INTERPRET_OEM_DATA);
  assert (wlen);
  assert (system_event_record_data);
  assert (system_event_record_data->event_type_code == IPMI_EVENT_READING_TYPE_CODE_SENSOR_SPECIFIC);

  /* OEM Interpretation
   *
   * Fujitsu iRMC / iRMC S2
   */
  if ((ctx->product_id >= IPMI_FUJITSU_PRODUCT_ID_MIN
       && ctx->product_id <= IPMI_FUJITSU_PRODUCT_ID_MAX)
      && (system_event_record_data->sensor_type == IPMI_SENSOR_TYPE_OEM_FUJITSU_I2C_BUS
          || system_event_record_data->sensor_type == IPMI_SENSOR_TYPE_OEM_FUJITSU_SYSTEM_POWER_CONSUMPTION
          || system_event_record_data->sensor_type == IPMI_SENSOR_TYPE_OEM_FUJITSU_MEMORY_STATUS
          || system_event_record_data->sensor_type == IPMI_SENSOR_TYPE_OEM_FUJITSU_MEMORY_CONFIG
          || system_event_record_data->sensor_type == IPMI_SENSOR_TYPE_OEM_FUJITSU_MEMORY
          || system_event_record_data->sensor_type == IPMI_SENSOR_TYPE_OEM_FUJITSU_HW_ERROR
          || system_event_record_data->sensor_type == IPMI_SENSOR_TYPE_OEM_FUJITSU_SYS_ERROR
          || system_event_record_data->sensor_type == IPMI_SENSOR_TYPE_OEM_FUJITSU_FAN_STATUS
          || system_event_record_data->sensor_type == IPMI_SENSOR_TYPE_OEM_FUJITSU_PSU_STATUS
          || system_event_record_data->sensor_type == IPMI_SENSOR_TYPE_OEM_FUJITSU_PSU_REDUNDANCY
          || system_event_record_data->sensor_type == IPMI_SENSOR_TYPE_OEM_FUJITSU_FLASH
          || system_event_record_data->sensor_type == IPMI_SENSOR_TYPE_OEM_FUJITSU_CONFIG_BACKUP))
    {
      int ret;

      ret = ipmi_get_oem_sensor_type_message (ctx->manufacturer_id,
                                              ctx->product_id,
                                              system_event_record_data->sensor_type,
                                              system_event_record_data->offset_from_event_reading_type_code,
                                              tmpbuf,
                                              tmpbuflen);

      if (ret > 0)
        return (1);
    }
  
  return (0);
}

/* Holger: XXX: below implementation only calls get-long-text if
 * event-data2 AND event-data3 are OEM extensions
 *
 * if there is a possibility of:
 *
 * event1-text ; event2-text; OEM event-data3
 *
 * or
 *
 * event1-text ; OEM event-data2 ; event3-text
 *
 * Then we'll need some more callbacks for those specific circumstances
 */

/* return (0) - no OEM match
 * return (1) - OEM match
 * return (-1) - error, cleanup and return error
 *
 * in oem_rv, return
 * 0 - continue on
 * 1 - buffer full, return full buffer to user
 */
int
ipmi_sel_parse_output_fujitsu_event_data2_event_data3 (ipmi_sel_parse_ctx_t ctx,
							struct ipmi_sel_parse_entry *sel_parse_entry,
							uint8_t sel_record_type,
							char *buf,
							unsigned int buflen,
							unsigned int flags,
							unsigned int *wlen,
							struct ipmi_sel_system_event_record_data *system_event_record_data,
							int *oem_rv)
{
  assert (ctx);
  assert (ctx->magic == IPMI_SEL_PARSE_CTX_MAGIC);
  assert (ctx->manufacturer_id == IPMI_IANA_ENTERPRISE_ID_FUJITSU);
  assert (sel_parse_entry);
  assert (buf);
  assert (buflen);
  assert (!(flags & ~IPMI_SEL_PARSE_STRING_MASK));
  assert (flags & IPMI_SEL_PARSE_STRING_FLAGS_INTERPRET_OEM_DATA);
  assert (wlen);
  assert (system_event_record_data);
  assert (oem_rv);

  /* OEM Interpretation
   *
   * Fujitsu iRMC / iRMC S2
   */
  if ((ctx->product_id >= IPMI_FUJITSU_PRODUCT_ID_MIN
       && ctx->product_id <= IPMI_FUJITSU_PRODUCT_ID_MAX)
      && system_event_record_data->event_data2_flag == IPMI_SEL_EVENT_DATA_OEM_CODE
      && system_event_record_data->event_data3_flag == IPMI_SEL_EVENT_DATA_OEM_CODE)
    {
      char selbuf[IPMI_OEM_FUJITSU_SEL_ENTRY_LONG_TEXT_MAX_STRING_LENGTH + 1];
      
      memset (selbuf, '\0', IPMI_OEM_FUJITSU_SEL_ENTRY_LONG_TEXT_MAX_STRING_LENGTH + 1);
      
      /* don't output severity since this is not event data 1 */
      if (_ipmi_sel_oem_fujitsu_get_sel_entry_long_text (ctx,
                                                         sel_parse_entry,
                                                         selbuf,
                                                         IPMI_OEM_FUJITSU_SEL_ENTRY_LONG_TEXT_MAX_STRING_LENGTH,
                                                         0) < 0)
        return (-1);
      
      if (strlen (selbuf))
        {
          if (ipmi_sel_parse_string_snprintf (buf,
                                              buflen,
                                              wlen,
                                              "%s",
                                              selbuf))
            (*oem_rv) = 1;
          else
            (*oem_rv) = 0;
          
          return (1);
        }
    }

  return (0);
}

/* return (0) - no OEM match
 * return (1) - OEM match
 * return (-1) - error, cleanup and return error
 *
 * in oem_rv, return
 * 0 - continue on
 * 1 - buffer full, return full buffer to user
 */
int
ipmi_sel_parse_output_fujitsu_oem_record_data (ipmi_sel_parse_ctx_t ctx,
                                               struct ipmi_sel_parse_entry *sel_parse_entry,
                                               uint8_t sel_record_type,
                                               char *buf,
                                               unsigned int buflen,
                                               unsigned int flags,
                                               unsigned int *wlen,
                                               int *oem_rv)
{
  assert (ctx);
  assert (ctx->magic == IPMI_SEL_PARSE_CTX_MAGIC);
  assert (ctx->manufacturer_id == IPMI_IANA_ENTERPRISE_ID_FUJITSU);
  assert (ipmi_sel_record_type_class (sel_record_type) == IPMI_SEL_RECORD_TYPE_CLASS_TIMESTAMPED_OEM_RECORD
          || ipmi_sel_record_type_class (sel_record_type) == IPMI_SEL_RECORD_TYPE_CLASS_NON_TIMESTAMPED_OEM_RECORD);
  assert (sel_parse_entry);
  assert (buf);
  assert (buflen);
  assert (!(flags & ~IPMI_SEL_PARSE_STRING_MASK));
  assert (flags & IPMI_SEL_PARSE_STRING_FLAGS_INTERPRET_OEM_DATA);
  assert (wlen);
  assert (oem_rv);

  /* OEM Interpretation
   *
   * Fujitsu iRMC / iRMC S2
   */
  if ((ctx->product_id >= IPMI_FUJITSU_PRODUCT_ID_MIN
       && ctx->product_id <= IPMI_FUJITSU_PRODUCT_ID_MAX))
    {
      char selbuf[IPMI_OEM_FUJITSU_SEL_ENTRY_LONG_TEXT_MAX_STRING_LENGTH + 1];

      memset (selbuf, '\0', IPMI_OEM_FUJITSU_SEL_ENTRY_LONG_TEXT_MAX_STRING_LENGTH + 1);

      if (_ipmi_sel_oem_fujitsu_get_sel_entry_long_text (ctx,
                                                         sel_parse_entry,
                                                         selbuf,
                                                         IPMI_OEM_FUJITSU_SEL_ENTRY_LONG_TEXT_MAX_STRING_LENGTH,
                                                         1) < 0)
        return (-1);

      if (strlen (selbuf))
        {
          if (ipmi_sel_parse_string_snprintf (buf,
                                              buflen,
                                              wlen,
                                              "%s",
                                              selbuf))
            (*oem_rv) = 1;
          else
            (*oem_rv) = 0;
          
          return (1);
        }
    }

  return (0);
}
