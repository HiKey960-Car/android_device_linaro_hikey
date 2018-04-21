/*
 * Generated by asn1c-0.9.22 (http://lionet.info/asn1c)
 * From ASN.1 module "RRLP-Components"
 * 	found in "rrlp-components.asn"
 * 	`asn1c -gen-PER -fnative-types`
 */

#ifndef	_AssistBTSData_H_
#define	_AssistBTSData_H_


#include <asn_application.h>

/* Including external dependencies */
#include "BSIC.h"
#include "MultiFrameOffset.h"
#include "TimeSlotScheme.h"
#include "RoughRTD.h"
#include <constr_SEQUENCE.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
struct CalcAssistanceBTS;

/* AssistBTSData */
typedef struct AssistBTSData {
	BSIC_t	 bsic;
	MultiFrameOffset_t	 multiFrameOffset;
	TimeSlotScheme_t	 timeSlotScheme;
	RoughRTD_t	 roughRTD;
	struct CalcAssistanceBTS	*calcAssistanceBTS	/* OPTIONAL */;
	
	/* Context for parsing across buffer boundaries */
	asn_struct_ctx_t _asn_ctx;
} AssistBTSData_t;

/* Implementation */
extern asn_TYPE_descriptor_t asn_DEF_AssistBTSData;

#ifdef __cplusplus
}
#endif

/* Referred external types */
#include "CalcAssistanceBTS.h"

#endif	/* _AssistBTSData_H_ */
#include <asn_internal.h>
