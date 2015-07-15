#include <grub/err.h>
#include <grub/i18n.h>
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/tpm.h>
#include <grub/term.h>
#include <grub/verify.h>
#include <grub/dl.h>

GRUB_MOD_LICENSE ("GPLv3+")

grub_err_t
grub_tpm_measure (unsigned char *buf, grub_size_t size, grub_uint8_t pcr,
		  const char *description)
{
  return grub_tpm_log_event (buf, size, pcr, description);
}

static grub_err_t
grub_tpm_verify_init (grub_file_t io,
		      enum grub_file_type type __attribute__ ((unused)),
		      void **context, enum grub_verify_flags *flags)
{
  *context = io->name;
  *flags |= GRUB_VERIFY_FLAGS_SINGLE_CHUNK;
  return GRUB_ERR_NONE;
}

static grub_err_t
grub_tpm_verify_write (void *context, void *buf, grub_size_t size)
{
  return grub_tpm_measure (buf, size, 9, context);
}

static void
grub_tpm_verify_close (void *ctxt __attribute__ ((unused)))
{
  return;
}

static grub_err_t
grub_tpm_verify_string (char *str, enum grub_verify_string_type type)
{
  const char *prefix = NULL;
  char *description;
  grub_err_t status;

  switch (type)
    {
    case GRUB_VERIFY_KERNEL_CMDLINE:
      prefix = "kernel_cmdline: ";
      break;
    case GRUB_VERIFY_MODULE_CMDLINE:
      prefix = "module_cmdline: ";
      break;
    case GRUB_VERIFY_COMMAND:
      prefix = "grub_cmd: ";
      break;
    }
  description = grub_malloc(grub_strlen(str) + grub_strlen(prefix) + 1);
  if (!description)
    return grub_errno;
  grub_memcpy(description, prefix, grub_strlen(prefix));
  grub_memcpy(description + grub_strlen(prefix), str, grub_strlen(str) + 1);
  status = grub_tpm_measure ((unsigned char *) str, grub_strlen (str), 8,
			     description);
  grub_free(description);
  return status;
}

struct grub_file_verifier grub_tpm_verifier = {
  .name = "tpm",
  .init = grub_tpm_verify_init,
  .write = grub_tpm_verify_write,
  .close = grub_tpm_verify_close,
  .verify_string = grub_tpm_verify_string,
};

GRUB_MOD_INIT(tpm)
{
  grub_verifier_register (&grub_tpm_verifier);
}

GRUB_MOD_FINI(tpm)
{
  grub_verifier_unregister (&grub_tpm_verifier);
}
