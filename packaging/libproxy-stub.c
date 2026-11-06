/*
 * Minimal stand-in for libproxy.so.1, bundled in the AppImage.
 *
 * Ubuntu's QtNetwork links libproxy, which in turn loads curl, GnuTLS, Kerberos, LDAP,
 * libssh and a JavaScript engine (for PAC files) into every process, roughly 30 libraries
 * that a music player never uses. QtNetwork only needs these three functions, so this
 * version answers from the standard proxy environment variables instead:
 * http_proxy / https_proxy / all_proxy (and upper-case forms), honouring no_proxy.
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int unused; } pxProxyFactory;

pxProxyFactory *px_proxy_factory_new(void)
{
    return calloc(1, sizeof(pxProxyFactory));
}

void px_proxy_factory_free(pxProxyFactory *self) { free(self); }

static const char *env2(const char *a, const char *b)
{
    const char *v = getenv(a);
    if (!v || !*v)
        v = getenv(b);
    return (v && *v) ? v : NULL;
}

static int host_matches(const char *host, size_t hlen, const char *pat, size_t plen)
{
    while (plen && *pat == '.') { pat++; plen--; }
    if (plen == 1 && *pat == '*')
        return 1;
    if (hlen < plen)
        return 0;
    if (strncasecmp(host + hlen - plen, pat, plen) != 0)
        return 0;
    return hlen == plen || host[hlen - plen - 1] == '.';
}

static int bypassed(const char *url)
{
    const char *np = env2("no_proxy", "NO_PROXY");
    const char *h = strstr(url, "://");
    if (!h)
        return 0;
    h += 3;
    const char *at = strpbrk(h, "@/");
    if (at && *at == '@')
        h = at + 1;
    size_t hlen = strcspn(h, ":/?#");
    if ((hlen == 9 && strncasecmp(h, "localhost", 9) == 0) || (hlen >= 4 && strncmp(h, "127.", 4) == 0) ||
        (hlen == 5 && strncmp(h, "[::1]", 5) == 0))
        return 1;
    if (!np)
        return 0;
    while (*np) {
        while (*np == ',' || isspace((unsigned char)*np))
            np++;
        size_t len = strcspn(np, ", \t");
        if (len && host_matches(h, hlen, np, len))
            return 1;
        np += len;
    }
    return 0;
}

char **px_proxy_factory_get_proxies(pxProxyFactory *self, const char *url)
{
    (void)self;
    const char *proxy = NULL;
    if (url && !bypassed(url)) {
        if (strncasecmp(url, "https:", 6) == 0)
            proxy = env2("https_proxy", "HTTPS_PROXY");
        else if (strncasecmp(url, "http:", 5) == 0)
            proxy = env2("http_proxy", "HTTP_PROXY");
        if (!proxy)
            proxy = env2("all_proxy", "ALL_PROXY");
    }
    char **out = calloc(2, sizeof(char *));
    if (!out)
        return NULL;
    if (proxy && !strstr(proxy, "://")) {
        size_t n = strlen(proxy) + 8;
        out[0] = malloc(n);
        if (out[0]) {
            strcpy(out[0], "http://");
            strcat(out[0], proxy);
        }
    } else {
        out[0] = strdup(proxy ? proxy : "direct://");
    }
    return out;
}

void px_proxy_factory_free_proxies(char **proxies)
{
    if (!proxies)
        return;
    for (char **p = proxies; *p; ++p)
        free(*p);
    free(proxies);
}
