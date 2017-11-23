// -*-Mode: C++;-*- // technically C99

// * BeginRiceCopyright *****************************************************
//
// --------------------------------------------------------------------------
// Part of HPCToolkit (hpctoolkit.org)
//
// Information about sources of support for research and development of
// HPCToolkit is at 'hpctoolkit.org' and in 'README.Acknowledgments'.
// --------------------------------------------------------------------------
//
// Copyright ((c)) 2002-2017, Rice University
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// * Redistributions of source code must retain the above copyright
//   notice, this list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright
//   notice, this list of conditions and the following disclaimer in the
//   documentation and/or other materials provided with the distribution.
//
// * Neither the name of Rice University (RICE) nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// This software is provided by RICE and contributors "as is" and any
// express or implied warranties, including, but not limited to, the
// implied warranties of merchantability and fitness for a particular
// purpose are disclaimed. In no event shall RICE or contributors be
// liable for any direct, indirect, incidental, special, exemplary, or
// consequential damages (including, but not limited to, procurement of
// substitute goods or services; loss of use, data, or profits; or
// business interruption) however caused and on any theory of liability,
// whether in contract, strict liability, or tort (including negligence
// or otherwise) arising in any way out of the use of this software, even
// if advised of the possibility of such damage.
//
// ******************************************************* EndRiceCopyright *


/******************************************************************************
 * includes
 *****************************************************************************/

#include <linux/version.h>

/******************************************************************************
 * local includes
 *****************************************************************************/

#include <hpcrun/cct_insert_backtrace.h>
#include <hpcrun/utilities/tokenize.h>

#include <include/linux_info.h>
#include "perf-util.h"

// -----------------------------------------------------
// precise ip / skid options
// -----------------------------------------------------

// Possible value of precise ip:
//  0  SAMPLE_IP can have arbitrary skid.
//  1  SAMPLE_IP must have constant skid.
//  2  SAMPLE_IP requested to have 0 skid.
//  3  SAMPLE_IP must have 0 skid.
//  4  Detect automatically to have the most precise possible (default)
#define HPCRUN_OPTION_PRECISE_IP "HPCRUN_PRECISE_IP"

// default option for precise_ip: autodetect skid
#define PERF_EVENT_AUTODETECT_SKID    4

// constants of precise_ip (see the man page)
#define PERF_EVENT_SKID_ZERO_REQUIRED    3
#define PERF_EVENT_SKID_ZERO_REQUESTED   2
#define PERF_EVENT_SKID_CONSTANT         1
#define PERF_EVENT_SKID_ARBITRARY        0

#define HPCRUN_OPTION_PERF_COUNT "HPCRUN_PERF_COUNT"

// default the number of samples per second
// linux perf tool has default of 4000. It looks very high but
// visually the overhead is still small for them.
// however, for some machines, the overhead is significant, and
//  somehow it causes the kernel to adjust the period threshold to
//  less than 100.
// 300 samples per sec looks has relatively similar percentage
// with perf tool
#define DEFAULT_THRESHOLD  300

//******************************************************************************
// constants
//******************************************************************************

// ordered in increasing precision
const int perf_skid_precision[] = {
  PERF_EVENT_SKID_ARBITRARY,
  PERF_EVENT_SKID_CONSTANT,
  PERF_EVENT_SKID_ZERO_REQUESTED,
  PERF_EVENT_SKID_ZERO_REQUIRED
};

const int perf_skid_flavors = sizeof(perf_skid_precision)/sizeof(int);


//******************************************************************************
// local variables
//******************************************************************************


static uint16_t perf_kernel_lm_id;


//******************************************************************************
// forward declaration
//******************************************************************************



//******************************************************************************
// implementation
//******************************************************************************


#if 0
//
// add user and kernel callchain
//
static cct_node_t *
perf_add_callchain( cct_node_t        *parent_original, 
		    perf_mmap_data_t  *data, 
		    int               metric_id,
		    cct_metric_data_t datum )
{
  cct_node_t *parent = parent_original;
  
  // ------------------------------------------------------------------
  // insert cct
  // ------------------------------------------------------------------
  if (data != NULL && data->nr > 0) {
    // add kernel IPs to the call chain top down, which is the 
    // reverse of the order in which they appear in ips
    for (int i = data->nr - 1; i >= 0; i--) {

      uint16_t lm_id = perf_kernel_lm_id;
      // ------------------------------------------------------------------
      // case if we want to include user call chain as well:
      // Wild assumption : if hpcrun cannot find the load module of a certain
      //   address, it is possible the address is from the kernel
      // ------------------------------------------------------------------
      void *addr = (void *) data->ips[i];
      load_module_t *lm = hpcrun_loadmap_findByAddr(addr, addr+1);
      if (lm != NULL) lm_id = lm->id;

      // ------------------------------------------------------------------
      ip_normalized_t npc = { .lm_id = lm_id, .lm_ip = data->ips[i] };
      cct_addr_t frm = { .ip_norm = npc };
      cct_node_t *child = hpcrun_cct_insert_addr(parent, &frm);
      parent = child;
    }
  }
  // ------------------------------------------------------------------
  // adding metric to the leaf
  // ------------------------------------------------------------------
  metric_set_t* mset = hpcrun_reify_metric_set(parent);

  metric_upd_proc_t* upd_proc = hpcrun_get_metric_proc(metric_id);
  if (upd_proc) {
    upd_proc(metric_id, mset, datum);
  }
  return parent;
}
#endif

uint16_t
get_perf_kernel_lm_id()
{
  return perf_kernel_lm_id;
}

/*
 * get int long value of variable environment.
 * If the variable is not set, return the default value 
 */
static long
getEnvLong(const char *env_var, long default_value)
{
  const char *str_val= getenv(env_var);

  if (str_val) {
    char *end_ptr;
    long val = strtol( str_val, &end_ptr, 10 );
    if ( end_ptr != env_var && (val < LONG_MAX && val > LONG_MIN) ) {
      return val;
    }
  }
  // invalid value
  return default_value;
}


//----------------------------------------------------------
// find the best precise ip value in this platform
// @param current perf event attribute. This attribute can be
//    updated for the default precise ip.
// @return the assigned precise ip 
//----------------------------------------------------------
u64
get_precise_ip(struct perf_event_attr *attr)
{
  static int precise_ip = -1;

  // check if already computed
  if (precise_ip >= 0)
    return precise_ip;

  // check if user wants a specific ip-precision
  int val = getEnvLong(HPCRUN_OPTION_PRECISE_IP, PERF_EVENT_AUTODETECT_SKID);
  if (val >= PERF_EVENT_SKID_ARBITRARY && val <= PERF_EVENT_SKID_ZERO_REQUIRED)
  {
    attr->precise_ip = val;

    // check the validity of the requested precision
    // if it returns -1 we need to use our own auto-detect precision
    int ret = perf_event_open(attr,
            THREAD_SELF, CPU_ANY,
            GROUP_FD, PERF_FLAGS);
    if (ret >= 0) {
      precise_ip = val;
      return precise_ip;
    }
    EMSG("The kernel does not support the requested ip-precision: %d. hpcrun will use auto-detect ip-precision instead.", val);
  }

  // start with the most restrict skid (3) then 2, 1 and 0
  // this is specified in perf_event_open man page
  // if there's a change in the specification, we need to change
  // this one too (unfortunately)
  for(int i=perf_skid_flavors-1; i>=0; i--) {
    attr->precise_ip = perf_skid_precision[i];

    // ask sys to "create" the event
    // it returns -1 if it fails.
    int ret = perf_event_open(attr,
            THREAD_SELF, CPU_ANY,
            GROUP_FD, PERF_FLAGS);
    if (ret >= 0) {
      close(ret);
      precise_ip = i;
      // just quit when the returned value is correct
      return i;
    }
  }
  precise_ip = 0;
  return precise_ip;
}


//----------------------------------------------------------
// predicates that test perf availability
//----------------------------------------------------------

static int
perf_kernel_syms_avail()
{
  FILE *pe_paranoid = fopen(LINUX_PERF_EVENTS_FILE, "r");
  FILE *ksyms       = fopen(LINUX_KERNEL_SYMBOL_FILE, "r");
  int level         = 3; // default : no access to perf event

  if (ksyms != NULL && pe_paranoid != NULL) {
    fscanf(pe_paranoid, "%d", &level) ;
  }
  if (ksyms)       fclose(ksyms);
  if (pe_paranoid) fclose(pe_paranoid);

  return level;
}


//----------------------------------------------------------
// returns the maximum sample rate of this node
// based on info provided by LINUX_PERF_EVENTS_MAX_RATE file
//----------------------------------------------------------
static int
perf_max_sample_rate()
{
  FILE *perf_rate_file = fopen(LINUX_PERF_EVENTS_MAX_RATE, "r");
  int max_sample_rate  = -1;

  if (perf_rate_file != NULL) {
    fscanf(perf_rate_file, "%d", &max_sample_rate);
    fclose(perf_rate_file);
  }
  return max_sample_rate;
}


//----------------------------------------------------------
// Interface to see if the kernel symbol is available
// this function caches the value so that we don't need
//   enquiry the same question all the time.
//----------------------------------------------------------
static bool
is_perf_ksym_available()
{
  enum perf_ksym_e {PERF_UNDEFINED, PERF_AVAILABLE, PERF_UNAVAILABLE} ;

  // if kernel symbols are available, we will attempt to collect kernel
  // callchains and add them to our call paths
  static enum perf_ksym_e ksym_status = PERF_UNDEFINED;

  if (ksym_status == PERF_UNDEFINED) {
  	int level = perf_kernel_syms_avail();

    if (level == 0 || level == 1) {
      perf_kernel_lm_id = hpcrun_loadModule_add(LINUX_KERNEL_NAME);
      ksym_status = PERF_AVAILABLE;
    } else {
    	ksym_status = PERF_UNAVAILABLE;
    }
  }
  return (ksym_status == PERF_AVAILABLE);
}


/*************************************************************
 * Interface API
 **************************************************************/ 


/***
 * get the default event count threshold by looking from the environment variable
 * (HPCRUN_PERF_COUNT) set by hpcrun when user specifies -c option
 */
struct event_threshold_s
init_default_count()
{
  static struct event_threshold_s default_threshold = {.threshold_num=0, .threshold_type=FREQUENCY};
  if (default_threshold.threshold_num > 0)
    return default_threshold;

  const char *str_val= getenv(HPCRUN_OPTION_PERF_COUNT);
  if (str_val == NULL) {
    default_threshold.threshold_num = DEFAULT_THRESHOLD;
    return default_threshold;
  }

  int res = hpcrun_extract_threshold(str_val, &default_threshold.threshold_num, DEFAULT_THRESHOLD);
  if (res == 1)
    default_threshold.threshold_type = PERIOD;

  return default_threshold;
}



//----------------------------------------------------------
// generic default initialization for event attributes
// return true if the initialization is successful,
//   false otherwise.
//----------------------------------------------------------
int
perf_attr_init(
  struct perf_event_attr *attr,
  bool usePeriod, u64 threshold,
  u64  sampletype
)
{
  // by default, we always ask for sampling period information
  unsigned int sample_type = sampletype 
                             | PERF_SAMPLE_PERIOD | PERF_SAMPLE_TIME 
                             | PERF_SAMPLE_IP     | PERF_SAMPLE_ADDR 
                             | PERF_SAMPLE_CPU    | PERF_SAMPLE_TID;

  attr->size   = sizeof(struct perf_event_attr); /* Size of attribute structure */
  attr->freq   = (usePeriod ? 0 : 1);

  attr->sample_period = threshold;          /* Period or frequency of sampling     */
  int max_sample_rate = perf_max_sample_rate();

  if (attr->freq == 1 && threshold > max_sample_rate) {
    EMSG("WARNING: the rate %d is higher than the supported sample rate %d. Adjusted to the max rate.",
          threshold, max_sample_rate);
    attr->sample_period = max_sample_rate-1;
  }

  attr->disabled      = 1;                 /* the counter will be enabled later  */
  attr->sample_type   = sample_type;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,7,0)
  attr->exclude_callchain_user   = EXCLUDE_CALLCHAIN;
  attr->exclude_callchain_kernel = EXCLUDE_CALLCHAIN;
#endif

  attr->exclude_kernel = 1;
  attr->exclude_hv     = 1;
  attr->exclude_idle   = 1;

  if (is_perf_ksym_available()) {
    /* Records kernel call-chain when we have privilege */
    attr->sample_type             |= PERF_SAMPLE_CALLCHAIN;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,7,0)
    attr->exclude_callchain_kernel = INCLUDE_CALLCHAIN;
#endif
    attr->exclude_kernel 	   = 0;
    attr->exclude_hv         = 0;
    attr->exclude_idle       = 0;
  }
  attr->precise_ip    = get_precise_ip(attr);   /* the precision is either detected automatically
                                              as precise as possible or  on the user's variable.  */
  return true;
}
