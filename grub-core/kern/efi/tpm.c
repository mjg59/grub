#include <grub/err.h>
#include <grub/i18n.h>
#include <grub/efi/api.h>
#include <grub/efi/efi.h>
#include <grub/efi/tpm.h>
#include <grub/mm.h>
#include <grub/tpm.h>
#include <grub/term.h>

static grub_efi_guid_t tpm_guid = EFI_TPM_GUID;
static grub_efi_guid_t tpm2_guid = EFI_TPM2_GUID;

static grub_efi_boolean_t grub_tpm_present(grub_efi_tpm_protocol_t *tpm)
{
  grub_efi_status_t status;
  TCG_EFI_BOOT_SERVICE_CAPABILITY caps;
  grub_uint32_t flags;
  grub_efi_physical_address_t eventlog, lastevent;

  caps.Size = (grub_uint8_t)sizeof(caps);

  status = efi_call_5(tpm->status_check, tpm, &caps, &flags, &eventlog,
		      &lastevent);

  if (status != GRUB_EFI_SUCCESS || caps.TPMDeactivatedFlag
      || !caps.TPMPresentFlag)
    return 0;

  return 1;
}

static grub_efi_boolean_t grub_tpm2_present(grub_efi_tpm2_protocol_t *tpm)
{
  grub_efi_status_t status;
  EFI_TCG2_BOOT_SERVICE_CAPABILITY caps;

  caps.Size = (grub_uint8_t)sizeof(caps);

  status = efi_call_2(tpm->get_capability, tpm, &caps);

  if (status != GRUB_EFI_SUCCESS || !caps.TPMPresentFlag)
    return 0;

  return 1;
}

static grub_efi_boolean_t grub_tpm_handle_find(grub_efi_handle_t *tpm_handle,
					       grub_efi_uint8_t *protocol_version)
{
  grub_efi_handle_t *handles;
  grub_efi_uintn_t num_handles;

  handles = grub_efi_locate_handle (GRUB_EFI_BY_PROTOCOL, &tpm_guid, NULL,
				    &num_handles);
  if (handles && num_handles > 0) {
    *tpm_handle = handles[0];
    *protocol_version = 1;
    return 1;
  }

  handles = grub_efi_locate_handle (GRUB_EFI_BY_PROTOCOL, &tpm2_guid, NULL,
				    &num_handles);
  if (handles && num_handles > 0) {
    *tpm_handle = handles[0];
    *protocol_version = 2;
    return 1;
  }

  return 0;
}

grub_err_t
grub_tpm_execute(PassThroughToTPM_InputParamBlock *inbuf,
		 PassThroughToTPM_OutputParamBlock *outbuf)
{
  grub_efi_handle_t tpm_handle;
  grub_efi_status_t status;
  grub_efi_tpm_protocol_t *tpm;
  grub_efi_tpm2_protocol_t *tpm2;
  grub_uint8_t protocol_version;
  grub_uint32_t inhdrsize = sizeof(*inbuf) - sizeof(inbuf->TPMOperandIn);
  grub_uint32_t outhdrsize = sizeof(*outbuf) - sizeof(outbuf->TPMOperandOut);

  /* It's not a hard failure for there to be no TPM */
  if (!grub_tpm_handle_find(&tpm_handle, &protocol_version)) {
    return 0;
  }

  if (protocol_version == 1) {
    tpm = grub_efi_open_protocol (tpm_handle, &tpm_guid,
				GRUB_EFI_OPEN_PROTOCOL_GET_PROTOCOL);

    if (!grub_tpm_present(tpm)) {
      return 0;
    }

    /* UEFI TPM protocol takes the raw operand block, no param block header */
    status = efi_call_5 (tpm->pass_through_to_tpm, tpm,
			 inbuf->IPBLength - inhdrsize, inbuf->TPMOperandIn,
			 outbuf->OPBLength - outhdrsize, outbuf->TPMOperandOut);

    switch (status) {
    case GRUB_EFI_SUCCESS:
      return 0;
    case GRUB_EFI_DEVICE_ERROR:
      return grub_error (GRUB_ERR_IO, N_("Command failed"));
    case GRUB_EFI_INVALID_PARAMETER:
      return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("Invalid parameter"));
    case GRUB_EFI_BUFFER_TOO_SMALL:
      return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("Output buffer too small"));
    case GRUB_EFI_NOT_FOUND:
      return grub_error (GRUB_ERR_UNKNOWN_DEVICE, N_("TPM unavailable"));
    default:
      return grub_error (GRUB_ERR_UNKNOWN_DEVICE, N_("Unknown TPM error"));
    }
  } else {
    tpm2 = grub_efi_open_protocol (tpm_handle, &tpm2_guid,
				   GRUB_EFI_OPEN_PROTOCOL_GET_PROTOCOL);

    if (!grub_tpm2_present(tpm2)) {
      return 0;
    }

    /* UEFI TPM protocol takes the raw operand block, no param block header */
    status = efi_call_5 (tpm2->submit_command, tpm2,
			 inbuf->IPBLength - inhdrsize, inbuf->TPMOperandIn,
			 outbuf->OPBLength - outhdrsize, outbuf->TPMOperandOut);

    switch (status) {
    case GRUB_EFI_SUCCESS:
      return 0;
    case GRUB_EFI_DEVICE_ERROR:
      return grub_error (GRUB_ERR_IO, N_("Command failed"));
    case GRUB_EFI_INVALID_PARAMETER:
      return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("Invalid parameter"));
    case GRUB_EFI_BUFFER_TOO_SMALL:
      return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("Output buffer too small"));
    case GRUB_EFI_NOT_FOUND:
      return grub_error (GRUB_ERR_UNKNOWN_DEVICE, N_("TPM unavailable"));
    default:
      return grub_error (GRUB_ERR_UNKNOWN_DEVICE, N_("Unknown TPM error"));
    }
  }
}

typedef struct {
	grub_uint32_t pcrindex;
	grub_uint32_t eventtype;
	grub_uint8_t digest[20];
	grub_uint32_t eventsize;
	grub_uint8_t event[1];
} Event;

grub_err_t
grub_tpm_log_event(unsigned char *buf, grub_size_t size, grub_uint8_t pcr,
		   const char *description)
{
  grub_efi_handle_t tpm_handle;
  grub_efi_status_t status;
  grub_efi_tpm_protocol_t *tpm;
  grub_efi_tpm2_protocol_t *tpm2;
  grub_efi_physical_address_t lastevent;
  grub_efi_uint8_t protocol_version;
  grub_uint32_t eventnum = 0;
  grub_uint32_t algorithm;
  Event *event;
  EFI_TCG2_EVENT *event2;

  if (!grub_tpm_handle_find(&tpm_handle, &protocol_version)) {
    return 0;
  }

  if (protocol_version == 1) {
    tpm = grub_efi_open_protocol (tpm_handle, &tpm_guid,
				  GRUB_EFI_OPEN_PROTOCOL_GET_PROTOCOL);

    if (!grub_tpm_present(tpm)) {
      return 0;
    }

    event = grub_zalloc(sizeof (Event) + grub_strlen(description) + 1);
    if (!event) {
      grub_printf("No buffer\n");
      return grub_error (GRUB_ERR_OUT_OF_MEMORY,
			 N_("cannot allocate TPM event buffer"));
    }

    event->pcrindex = pcr;
    event->eventtype = 0x0d;
    event->eventsize = grub_strlen(description) + 1;
    grub_memcpy(event->event, description, event->eventsize);

    algorithm = 0x00000004; /* SHA 1 */
    status = efi_call_7 (tpm->log_extend_event, tpm, buf, (grub_uint64_t) size,
			 algorithm, event, &eventnum, &lastevent);

    switch (status) {
    case GRUB_EFI_SUCCESS:
      return 0;
    case GRUB_EFI_DEVICE_ERROR:
      return grub_error (GRUB_ERR_IO, N_("Command failed"));
    case GRUB_EFI_INVALID_PARAMETER:
      return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("Invalid parameter"));
    case GRUB_EFI_BUFFER_TOO_SMALL:
      return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("Output buffer too small"));
    case GRUB_EFI_NOT_FOUND:
      return grub_error (GRUB_ERR_UNKNOWN_DEVICE, N_("TPM unavailable"));
    default:
      return grub_error (GRUB_ERR_UNKNOWN_DEVICE, N_("Unknown TPM error"));
    }
  } else {
    tpm2 = grub_efi_open_protocol (tpm_handle, &tpm2_guid,
				   GRUB_EFI_OPEN_PROTOCOL_GET_PROTOCOL);

    if (!grub_tpm2_present(tpm2)) {
      return 0;
    }

    event2 = grub_zalloc(sizeof (Event) + grub_strlen(description) + 1);
    if (!event2) {
      grub_printf("No buffer\n");
      return grub_error (GRUB_ERR_OUT_OF_MEMORY,
			 N_("cannot allocate TPM event buffer"));
    }

    event2->Header.HeaderSize = sizeof(EFI_TCG2_EVENT_HEADER);
    event2->Header.HeaderVersion = 1;
    event2->Header.PCRIndex = pcr;
    event2->Header.EventType = 0x0d;
    event2->Size = sizeof(*event2) - sizeof(event2->Event) + grub_strlen(description) + 1;
    grub_memcpy(event2->Event, description, grub_strlen(description) + 1);

    status = efi_call_5 (tpm2->hash_log_extend_event, tpm2, 0, buf,
			 (grub_uint64_t) size, event2);

    switch (status) {
    case GRUB_EFI_SUCCESS:
      return 0;
    case GRUB_EFI_DEVICE_ERROR:
      return grub_error (GRUB_ERR_IO, N_("Command failed"));
    case GRUB_EFI_INVALID_PARAMETER:
      return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("Invalid parameter"));
    case GRUB_EFI_BUFFER_TOO_SMALL:
      return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("Output buffer too small"));
    case GRUB_EFI_NOT_FOUND:
      return grub_error (GRUB_ERR_UNKNOWN_DEVICE, N_("TPM unavailable"));
    default:
      return grub_error (GRUB_ERR_UNKNOWN_DEVICE, N_("Unknown TPM error"));
    }
  }
}
