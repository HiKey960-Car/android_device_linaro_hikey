/*
 * Generated by asn1c-0.9.22 (http://lionet.info/asn1c)
 * From ASN.1 module "ULP-Components"
 * 	found in "supl.asn"
 * 	`asn1c -gen-PER -fnative-types`
 */

#ifndef	_Version_H_
#define	_Version_H_


#include <asn_application.h>

/* Including external dependencies */
#include <NativeInteger.h>
#include <constr_SEQUENCE.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Version */
typedef struct Version {
	long	 maj;
	long	 min;
	long	 servind;
	
	/* Context for parsing across buffer boundaries */
	asn_struct_ctx_t _asn_ctx;
} Version_t;

/* Implementation */
extern asn_TYPE_descriptor_t asn_DEF_Version;

#ifdef __cplusplus
}
#endif

#endif	/* _Version_H_ */
#include <asn_internal.h>
