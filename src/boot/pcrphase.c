/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <getopt.h>

#include <sd-messages.h>

#include "build.h"
#include "efivars.h"
#include "env-util.h"
#include "main-func.h"
#include "openssl-util.h"
#include "parse-util.h"
#include "pretty-print.h"
#include "tpm-pcr.h"
#include "tpm2-util.h"

static bool arg_graceful = false;
static char *arg_tpm2_device = NULL;
static char **arg_banks = NULL;

STATIC_DESTRUCTOR_REGISTER(arg_banks, strv_freep);
STATIC_DESTRUCTOR_REGISTER(arg_tpm2_device, freep);

static int help(int argc, char *argv[], void *userdata) {
        _cleanup_free_ char *link = NULL;
        int r;

        r = terminal_urlify_man("systemd-pcrphase", "1", &link);
        if (r < 0)
                return log_oom();

        printf("%1$s  [OPTIONS...] WORD ...\n"
               "\n%5$sMeasure boot phase into TPM2 PCR 11.%6$s\n"
               "\n%3$sOptions:%4$s\n"
               "  -h --help              Show this help\n"
               "     --version           Print version\n"
               "     --bank=DIGEST       Select TPM bank (SHA1, SHA256)\n"
               "     --tpm2-device=PATH  Use specified TPM2 device\n"
               "     --graceful          Exit gracefully if no TPM2 device is found\n"
               "\nSee the %2$s for details.\n",
               program_invocation_short_name,
               link,
               ansi_underline(),
               ansi_normal(),
               ansi_highlight(),
               ansi_normal());

        return 0;
}

static int parse_argv(int argc, char *argv[]) {
        enum {
                ARG_VERSION = 0x100,
                ARG_BANK,
                ARG_TPM2_DEVICE,
                ARG_GRACEFUL,
        };

        static const struct option options[] = {
                { "help",        no_argument,       NULL, 'h'             },
                { "version",     no_argument,       NULL, ARG_VERSION     },
                { "bank",        required_argument, NULL, ARG_BANK        },
                { "tpm2-device", required_argument, NULL, ARG_TPM2_DEVICE },
                { "graceful",    no_argument,       NULL, ARG_GRACEFUL    },
                {}
        };

        int c;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "h", options, NULL)) >= 0)
                switch (c) {

                case 'h':
                        help(0, NULL, NULL);
                        return 0;

                case ARG_VERSION:
                        return version();

                case ARG_BANK: {
                        const EVP_MD *implementation;

                        implementation = EVP_get_digestbyname(optarg);
                        if (!implementation)
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Unknown bank '%s', refusing.", optarg);

                        if (strv_extend(&arg_banks, EVP_MD_name(implementation)) < 0)
                                return log_oom();

                        break;
                }

                case ARG_TPM2_DEVICE: {
                        _cleanup_free_ char *device = NULL;

                        if (streq(optarg, "list"))
                                return tpm2_list_devices();

                        if (!streq(optarg, "auto")) {
                                device = strdup(optarg);
                                if (!device)
                                        return log_oom();
                        }

                        free_and_replace(arg_tpm2_device, device);
                        break;
                }

                case ARG_GRACEFUL:
                        arg_graceful = true;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached();
                }

        return 1;
}

static int determine_banks(struct tpm2_context *c) {
        _cleanup_strv_free_ char **l = NULL;
        int r;

        assert(c);

        if (!strv_isempty(arg_banks)) /* Explicitly configured? Then use that */
                return 0;

        r = tpm2_get_good_pcr_banks_strv(c->esys_context, UINT32_C(1) << TPM_PCR_INDEX_KERNEL_IMAGE, &l);
        if (r < 0)
                return r;

        strv_free_and_replace(arg_banks, l);
        return 0;
}

static int run(int argc, char *argv[]) {
        _cleanup_(tpm2_context_destroy) struct tpm2_context c = {};
        _cleanup_free_ char *joined = NULL, *pcr_string = NULL;
        const char *word;
        unsigned pcr_nr;
        size_t length;
        int r;

        log_setup();

        r = parse_argv(argc, argv);
        if (r <= 0)
                return r;

        if (optind+1 != argc)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Expected a single argument.");

        word = argv[optind];

        /* Refuse to measure an empty word. We want to be able to write the series of measured words
         * separated by colons, where multiple separating colons are collapsed. Thus it makes sense to
         * disallow an empty word to avoid ambiguities. */
        if (isempty(word))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "String to measure cannot be empty, refusing.");

        if (arg_graceful && tpm2_support() != TPM2_SUPPORT_FULL) {
                log_notice("No complete TPM2 support detected, exiting gracefully.");
                return EXIT_SUCCESS;
        }

        length = strlen(word);

        int b = getenv_bool("SYSTEMD_PCRPHASE_STUB_VERIFY");
        if (b < 0 && b != -ENXIO)
                log_warning_errno(b, "Unable to parse $SYSTEMD_PCRPHASE_STUB_VERIFY value, ignoring.");

        /* Skip logic if sd-stub is not used, after all PCR 11 might have a very different purpose then. */
        r = efi_get_variable_string(EFI_LOADER_VARIABLE(StubPcrKernelImage), &pcr_string);
        if (r == -ENOENT) {
                if (b != 0) {
                        log_info("Kernel stub did not measure kernel image into PCR %u, skipping measurement.", TPM_PCR_INDEX_KERNEL_IMAGE);
                        return EXIT_SUCCESS;
                } else
                        log_notice("Kernel stub did not measure kernel image into PCR %u, but told to measure anyway, hence proceeding.", TPM_PCR_INDEX_KERNEL_IMAGE);
        } else if (r < 0)
                return log_error_errno(r, "Failed to read StubPcrKernelImage EFI variable: %m");
        else {
                /* Let's validate that the stub announced PCR 11 as we expected. */
                r = safe_atou(pcr_string, &pcr_nr);
                if (r < 0)
                        return log_error_errno(r, "Failed to parse StubPcrKernelImage EFI variable: %s", pcr_string);
                if (pcr_nr != TPM_PCR_INDEX_KERNEL_IMAGE) {
                        if (b != 0)
                                return log_error_errno(SYNTHETIC_ERRNO(EREMOTE), "Kernel stub measured kernel image into PCR %u, which is different than expected %u.", pcr_nr, TPM_PCR_INDEX_KERNEL_IMAGE);
                        else
                                log_notice("Kernel stub measured kernel image into PCR %u, which is different than expected %u, but told to measure anyway, hence proceeding.", pcr_nr, TPM_PCR_INDEX_KERNEL_IMAGE);
                } else
                        log_debug("Kernel stub reported same PCR %u as we want to use, proceeding.", TPM_PCR_INDEX_KERNEL_IMAGE);
        }

        r = dlopen_tpm2();
        if (r < 0)
                return log_error_errno(r, "Failed to load TPM2 libraries: %m");

        r = tpm2_context_init(arg_tpm2_device, &c);
        if (r < 0)
                return r;

        r = determine_banks(&c);
        if (r < 0)
                return r;
        if (strv_isempty(arg_banks)) /* Still none? */
                return log_error_errno(SYNTHETIC_ERRNO(ENOENT), "Found a TPM2 without enabled PCR banks. Can't operate.");

        joined = strv_join(arg_banks, ", ");
        if (!joined)
                return log_oom();

        log_debug("Measuring '%s' into PCR index %u, banks %s.", word, TPM_PCR_INDEX_KERNEL_IMAGE, joined);

        r = tpm2_extend_bytes(c.esys_context, arg_banks, TPM_PCR_INDEX_KERNEL_IMAGE, word, length); /* → PCR 11 */
        if (r < 0)
                return r;

        log_struct(LOG_INFO,
                   "MESSAGE_ID=" SD_MESSAGE_TPM_PCR_EXTEND_STR,
                   LOG_MESSAGE("Successfully extended PCR index %u with '%s' (banks %s).", TPM_PCR_INDEX_KERNEL_IMAGE, word, joined),
                   "MEASURING=%s", word,
                   "PCR=%u", TPM_PCR_INDEX_KERNEL_IMAGE,
                   "BANKS=%s", joined);

        return EXIT_SUCCESS;
}

DEFINE_MAIN_FUNCTION(run);
