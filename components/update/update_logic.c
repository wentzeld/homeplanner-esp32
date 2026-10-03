#include "update_logic.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static const char *skip_v(const char *s) { return s && (*s == 'v' || *s == 'V') ? s + 1 : (s ? s : ""); }

int update_version_cmp(const char *a, const char *b) {
    a = skip_v(a);
    b = skip_v(b);
    for (int part = 0; part < 4; part++) {
        long x = 0, y = 0;
        if (isdigit((unsigned char)*a)) x = strtol(a, (char **)&a, 10);
        if (isdigit((unsigned char)*b)) y = strtol(b, (char **)&b, 10);
        if (x != y) return x < y ? -1 : 1;
        a += *a == '.';
        b += *b == '.';
    }
    return 0;
}

static bool version_ok(const char *v) {
    if (!isdigit((unsigned char)*v)) return false;
    for (; *v; v++)
        if (!isdigit((unsigned char)*v) && *v != '.') return false;
    return true;
}

bool update_parse_release(const char *json, update_release_t *out) {
    memset(out, 0, sizeof *out);
    cJSON *root = cJSON_Parse(json ? json : "");
    bool ok = false;
    const cJSON *tag = cJSON_GetObjectItemCaseSensitive(root, "tag_name");
    if (!cJSON_IsObject(root) || !cJSON_IsString(tag) || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "draft")) ||
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "prerelease"))) {
        goto done;
    }
    snprintf(out->version, sizeof out->version, "%s", skip_v(tag->valuestring));
    if (!version_ok(out->version)) goto done;

    const cJSON *asset;
    cJSON_ArrayForEach(asset, cJSON_GetObjectItemCaseSensitive(root, "assets")) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(asset, "name");
        const cJSON *url = cJSON_GetObjectItemCaseSensitive(asset, "browser_download_url");
        const cJSON *size = cJSON_GetObjectItemCaseSensitive(asset, "size");
        if (cJSON_IsString(name) && strcmp(name->valuestring, UPDATE_ASSET_NAME) == 0 && cJSON_IsString(url) &&
            strncmp(url->valuestring, "https://", 8) == 0 && strlen(url->valuestring) < sizeof out->url) {
            snprintf(out->url, sizeof out->url, "%s", url->valuestring);
            out->size = cJSON_IsNumber(size) ? (long)size->valuedouble : 0;
        }
    }
    if (!out->url[0]) goto done;

    const cJSON *body = cJSON_GetObjectItemCaseSensitive(root, "body");
    if (cJSON_IsString(body)) {
        // Plain text: drop \r, keep at most the space we have (cut at a UTF-8 boundary).
        size_t o = 0;
        for (const char *p = body->valuestring; *p && o + 1 < sizeof out->notes; p++)
            if (*p != '\r') out->notes[o++] = *p;
        while (o && ((unsigned char)out->notes[o] & 0xC0) == 0x80) o--;
        out->notes[o] = '\0';
        while (o && isspace((unsigned char)out->notes[o - 1])) out->notes[--o] = '\0';
    }
    ok = true;
done:
    cJSON_Delete(root);
    if (!ok) memset(out, 0, sizeof *out);
    return ok;
}
