
#include "alias.h"
#include "driver.h"
#include "log.h"
#include "server.h"
#include "utils.h"

#include <bits/getopt_core.h>
#include <unistd.h>

#include <string.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdbool.h>

TXVC_DEFAULT_LOG_TAG(txvc);

static bool driver_usage(const struct txvc_driver *d, const void *extra) {
    TXVC_UNUSED(extra);
    printf("\"%s\":\n%s\n", d->name, d->help);
    return true;
}

static void print_available_aliases(void) {
    for (const struct txvc_profile_alias *alias = txvc_profile_aliases; alias->alias; alias++) {
        printf("\"%s\" - %s\n", alias->alias, alias->description);
    }
}

#define DEFAULT_SERVER_ADDR "127.0.0.1:2542"

#define CLI_OPTION_LIST_ITEMS(X) \
    X(const char *, serverAddr, "a", " <ipv4_address:port>",\
        "Colon-separated IPv4 address and port to listen for incoming XVC connections at" \
            " (default: " DEFAULT_SERVER_ADDR ")", optarg) \
    X(const char *, profile,    "p", " <profile_string_or_alias>",\
        "Server HW profile or profile alias, see below", optarg) \
    X(bool,         verbose,    "v", "",\
        "Enable verbose output", true) \
    X(bool,         help,       "h", "",\
        "Print this message", true) \

struct cli_options {
#define AS_STRUCT_FIELD(type, name, optChar, optArg, description, initializer) type name;
    CLI_OPTION_LIST_ITEMS(AS_STRUCT_FIELD)
#undef AS_STRUCT_FIELD
};

static void printUsage(const char *progname, bool detailed) {
#define AS_SYNOPSYS_ENTRY(type, name, optChar, optArg, description, initializer) \
    "\t\t[-" optChar optArg "]\n"
    const char *synopsysOptions = CLI_OPTION_LIST_ITEMS(AS_SYNOPSYS_ENTRY);
#undef AS_SYNOPSYS_ENTRY

#define AS_USAGE_ENTRY(type, name, optChar, optArg, description, initializer) \
    " -" optChar " - " description "\n" 
    const char *usageOptions = CLI_OPTION_LIST_ITEMS(AS_USAGE_ENTRY);
#undef AS_USAGE_ENTRY

    if (detailed) {
        printf("TinyXVC - minimalistic XVC (Xilinx Virtual Cable) server, v0.0\n");
    }
    printf("Usage: %s\n"
           "%s\n"
           "%s\n",
           progname, synopsysOptions, usageOptions);

    if (!detailed) {
        return;
    }

    printf("\tProfiles:\n");
    printf("HW profile is a specification that defines a backend to be used by server"
           " and its parameters. Backend here means a particular device that eventually"
           " receives and answers to XVC commands. HW profile is specified in the following form:\n"
           "\n\t<driver_name>:<arg0>=<val0>,<arg1>=<val1>,<arg2>=<val2>,...\n\n"
           "Available driver names as well as their specific parameters are listed below."
           " Also there are a few predefined profile aliases for specific HW that can be used"
           " instead of fully specified description, see below.\n\n");
    printf("\tDrivers:\n");
    txvc_enumerate_drivers(driver_usage, NULL);
    printf("\n");
    printf("\tAliases:\n");
    print_available_aliases();
    printf("\n");
}


static bool parse_cli_options(int argc, char **argv, struct cli_options *out) {
    char optstring[128];
#define AS_FORMAT_SPEC(type, name, optChar, optArg, description, initializer) \
    "%s"
#define AS_OPTSTR_ENTRY(type, name, optChar, optArg, description, initializer) \
    , (optArg[0] ? optChar ":" : optChar)

    snprintf(optstring, sizeof(optstring),
        CLI_OPTION_LIST_ITEMS(AS_FORMAT_SPEC)
        CLI_OPTION_LIST_ITEMS(AS_OPTSTR_ENTRY)
        );
#undef AS_OPTSTR_ENTRY
#undef AS_FORMAT_SPEC

    int opt;
    while ((opt = getopt(argc, argv, optstring)) != -1) {
#define APPLY_OPTION(type, name, optChar, optArg, description, initializer) \
        if (opt == optChar[0]) { \
            out->name = initializer; \
            continue; \
        }

        CLI_OPTION_LIST_ITEMS(APPLY_OPTION)

#undef APPLY_OPTION
        return false;
    }

    if (argv[optind] != NULL) {
        fprintf(stderr, "%s: unrecognized extra operands\n", argv[0]);
    }
    return argv[optind] == NULL;
}

static volatile sig_atomic_t shouldTerminate = 0;

static void sigint_handler(int signo) {
    TXVC_UNUSED(signo);
    ssize_t ignored __attribute__((unused));
    ignored = write(STDOUT_FILENO, "Terminating...\n", 15);
    shouldTerminate = 1;
}

static void listen_for_user_interrupt(void) {
    /*
     * Received SIGINT must NOT restart interrupted syscalls, so that server code will be able
     * to test termination flag in a timely manner.
     * Don't use signal() as it may force restarts.
     */
    struct sigaction sa;
    sa.sa_flags = 0; /* No SA_RESTART here */
    sigemptyset(&sa.sa_mask);
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdisabled-macro-expansion"
#endif
    sa.sa_handler = sigint_handler;
#ifdef __clang__
#pragma clang diagnostic pop
#endif
    sigaction(SIGINT, &sa, NULL);
}

static bool find_by_name(const struct txvc_driver *d, const void *extra) {
    const char* name = extra;
    return strcmp(name, d->name) != 0;
}

static const struct txvc_driver *activate_driver(const char *profile) {
    for (const struct txvc_profile_alias *alias = txvc_profile_aliases; alias->alias; alias++) {
        if (strcmp(profile, alias->alias) == 0) {
            INFO("Found alias %s (%s),\n", alias->alias, alias->description);
            INFO("using profile %s\n", alias->profile);
            profile = alias->profile;
        }
    }

    /*
     * Copy the whole profile string in a temporary buffer and cut it onto name,value chuncks.
     * Expected format is:
     * <driver name>:<name0>=<val0>,<name1>=<val1>,<name2>=<val2>,...
     */
    char args[1024];
    strncpy(args, profile, sizeof(args));
    args[sizeof(args) - 1] = '\0';

    const char *name = args;
    const char *argNames[32] = { NULL };
    const char *argValues[32] = { NULL };

    char* cur = strchr(args, ':');
    if (cur) {
        *cur++ = '\0';
        for (size_t i = 0; i < sizeof(argNames) / sizeof(argNames[0]) && cur && *cur; i++) {
            char* tmp = cur;
            cur = strchr(cur, ',');
            if (cur) {
                *cur++ = '\0';
            }
            argNames[i] = tmp;
            tmp = strchr(tmp, '=');
            if (tmp) {
                *tmp++ = '\0';
                argValues[i] = tmp;
            } else {
                argValues[i] = "";
            }
        }
    }

    const struct txvc_driver *d = txvc_enumerate_drivers(find_by_name, name);
    if (d) {
        if (!d->activate(argNames, argValues)) {
            ERROR("Failed to activate driver \"%s\"\n", name);
            d = NULL;
        }
    } else {
        ERROR("Can not find driver \"%s\"\n", name);
    }
    return d;
}

int main(int argc, char**argv) {
    struct cli_options opts = { 0 };
    if (!parse_cli_options(argc, argv, &opts)) {
        printUsage(argv[0], false);
        return EXIT_FAILURE;
    }

    if (opts.help) {
        printUsage(argv[0], true);
        return EXIT_SUCCESS;
    }
    if (!opts.profile) {
        fprintf(stderr, "Profile is missing\n");
        return EXIT_FAILURE;
    }
    if (!opts.serverAddr) {
        opts.serverAddr = DEFAULT_SERVER_ADDR;
    }
    txvc_set_log_min_level(opts.verbose ? LOG_LEVEL_VERBOSE : LOG_LEVEL_INFO);

    listen_for_user_interrupt();

    const struct txvc_driver *d = activate_driver(opts.profile);
    if (d) {
        txvc_run_server(opts.serverAddr, d, &shouldTerminate);
        if (!d->deactivate()) {
            WARN("Failed to deactivate driver \"%s\"\n", d->name);
        }
    }
    return 0;
}

