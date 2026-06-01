/*******************************************************************************
*                                                                              *
* diffHighlight.c -- Highlight unified-diff changes in an NEdit window          *
*                                                                              *
*******************************************************************************/
#ifdef HAVE_CONFIG_H
#include "../config.h"
#endif

#include "diffHighlight.h"
#include "rangeset.h"
#include "textBuf.h"
#include "undo.h"
#include "window.h"
#include "../util/nedit_malloc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIFF_ADD_COLOR "#d8ffd8"
#define DIFF_DEL_COLOR "#ffd8d8"

typedef struct {
    int start;
    int end;
} DiffRange;

typedef struct {
    DiffRange *ranges;
    int count;
    int capacity;
} DiffRangeList;

typedef struct {
    char *text;
    size_t length;
    size_t capacity;
} TextBuilder;

static char *readFile(const char *path);
static int buildDiffView(WindowInfo *window, const char *diffText,
        const char *openedFilePath, char **viewText,
        DiffRangeList *addRanges, DiffRangeList *delRanges);
static int parseHunkHeader(const char *line, int *oldStart, int *oldCount,
        int *newStart, int *newCount);
static int parseRange(const char **text, char marker, int *start, int *count);
static char *copyDiffPath(const char *text);
static int pathMatches(const char *diffPath, const char *openedFilePath,
        const WindowInfo *window);
static int pathEndsWith(const char *path, const char *suffix);
static const char *baseName(const char *path);
static const char *skipDotSlash(const char *path);
static const char *skipConstSpace(const char *text);
static void trimLineEnd(char *line);
static void initRangeList(DiffRangeList *list);
static void freeRangeList(DiffRangeList *list);
static void addDiffRange(DiffRangeList *list, int start, int end);
static int applyDiffRanges(Rangeset *set, const DiffRangeList *list);
static void initTextBuilder(TextBuilder *builder, size_t initialCapacity);
static void freeTextBuilder(TextBuilder *builder);
static void appendBytes(TextBuilder *builder, const char *text, size_t length);
static void appendDiffLine(TextBuilder *builder, const char *text);
static int appendSourceLine(TextBuilder *builder, const char *source,
        size_t *sourcePos);
static void advanceSourceLine(const char *source, size_t *sourcePos);
static void appendSourceLinesUntil(TextBuilder *builder, const char *source,
        size_t *sourcePos, int *sourceLine, int targetLine);
static void appendRemainingSource(TextBuilder *builder, const char *source,
        size_t *sourcePos);
static void appendDisplayedNewLine(TextBuilder *builder, const char *source,
        size_t *sourcePos, int *sourceLine, const char *fallback,
        DiffRangeList *ranges);
static void clearExistingDiffRanges(RangesetTable *table);

void ApplyDiffFileHighlight(WindowInfo *window, const char *diffFile,
        const char *openedFilePath)
{
    char *diffText;
    int addLabel, delLabel;
    Rangeset *addSet, *delSet;
    int addRanges = 0;
    int delRanges = 0;
    char *viewText = NULL;
    DiffRangeList addRangeList;
    DiffRangeList delRangeList;

    if (window == NULL || window->buffer == NULL || diffFile == NULL)
        return;

    initRangeList(&addRangeList);
    initRangeList(&delRangeList);

    diffText = readFile(diffFile);
    if (diffText == NULL)
        return;

    if (!buildDiffView(window, diffText, openedFilePath, &viewText,
                &addRangeList, &delRangeList)) {
        fprintf(stderr,
                "nedit: --diff-file: no matching hunks in %s for %s%s\n",
                diffFile, window->path, window->filename);
        NEditFree(diffText);
        freeRangeList(&addRangeList);
        freeRangeList(&delRangeList);
        return;
    }

    if (window->buffer->rangesetTable == NULL)
        window->buffer->rangesetTable = RangesetTableAlloc(window->buffer);
    if (window->buffer->rangesetTable == NULL) {
        fprintf(stderr, "nedit: --diff-file: cannot allocate rangesets\n");
        NEditFree(diffText);
        NEditFree(viewText);
        freeRangeList(&addRangeList);
        freeRangeList(&delRangeList);
        return;
    }

    clearExistingDiffRanges(window->buffer->rangesetTable);

    window->ignoreModify = True;
    BufSetAll(window->buffer, viewText);
    window->ignoreModify = False;
    ClearUndoList(window);
    ClearRedoList(window);
    SetWindowModified(window, False);
    SET_USER_LOCKED(window->lockReasons, True);
    SET_PERM_LOCKED(window->lockReasons, True);
    UpdateWindowReadOnly(window);
    UpdateWindowTitle(window);
    RefreshTabState(window);
    UpdateStatsLine(window);

    delLabel = RangesetCreate(window->buffer->rangesetTable);
    addLabel = RangesetCreate(window->buffer->rangesetTable);
    delSet = RangesetFetch(window->buffer->rangesetTable, delLabel);
    addSet = RangesetFetch(window->buffer->rangesetTable, addLabel);

    if (addSet == NULL || delSet == NULL) {
        fprintf(stderr, "nedit: --diff-file: no rangesets available\n");
        if (addSet != NULL)
            RangesetForget(window->buffer->rangesetTable, addLabel);
        if (delSet != NULL)
            RangesetForget(window->buffer->rangesetTable, delLabel);
        NEditFree(diffText);
        NEditFree(viewText);
        freeRangeList(&addRangeList);
        freeRangeList(&delRangeList);
        return;
    }

    RangesetAssignName(delSet, "diff-deletions");
    RangesetAssignColorName(delSet, DIFF_DEL_COLOR);
    RangesetChangeModifyResponse(delSet, "include");

    RangesetAssignName(addSet, "diff-additions");
    RangesetAssignColorName(addSet, DIFF_ADD_COLOR);
    RangesetChangeModifyResponse(addSet, "include");

    addRanges = applyDiffRanges(addSet, &addRangeList);
    delRanges = applyDiffRanges(delSet, &delRangeList);

    if (addRanges == 0)
        RangesetForget(window->buffer->rangesetTable, addLabel);
    if (delRanges == 0)
        RangesetForget(window->buffer->rangesetTable, delLabel);

    NEditFree(diffText);
    NEditFree(viewText);
    freeRangeList(&addRangeList);
    freeRangeList(&delRangeList);
}

static char *readFile(const char *path)
{
    FILE *fp;
    long len;
    size_t nRead;
    char *text;

    fp = fopen(path, "rb");
    if (fp == NULL) {
        fprintf(stderr, "nedit: cannot open diff file %s\n", path);
        return NULL;
    }

    if (fseek(fp, 0L, SEEK_END) != 0) {
        fprintf(stderr, "nedit: cannot seek diff file %s\n", path);
        fclose(fp);
        return NULL;
    }
    len = ftell(fp);
    if (len < 0) {
        fprintf(stderr, "nedit: cannot read diff file size %s\n", path);
        fclose(fp);
        return NULL;
    }
    rewind(fp);

    text = (char *)NEditMalloc((size_t)len + 1);
    nRead = fread(text, 1, (size_t)len, fp);
    fclose(fp);

    text[nRead] = '\0';
    return text;
}

static int buildDiffView(WindowInfo *window, const char *diffText,
        const char *openedFilePath, char **viewText,
        DiffRangeList *addRanges, DiffRangeList *delRanges)
{
    char *sourceText = BufGetAll(window->buffer);
    char *copy = NEditStrdup(diffText);
    char *line = copy;
    char *next;
    TextBuilder builder;
    size_t sourcePos = 0;
    int sourceLine = 1;
    int fileMatches = 0;
    int inHunk = 0;
    int oldStart, oldCount, newStart, newCount;
    int matchedFile = 0;

    initTextBuilder(&builder, strlen(sourceText) + strlen(diffText) + 1);

    while (line != NULL) {
        next = strchr(line, '\n');
        if (next != NULL) {
            *next = '\0';
            next++;
        }
        trimLineEnd(line);

        if (!strncmp(line, "diff --git ", 11) || !strncmp(line, "Index: ", 7)) {
            fileMatches = 0;
            inHunk = 0;
        } else if (!strncmp(line, "+++ ", 4)) {
            char *path = copyDiffPath(line + 4);
            fileMatches = pathMatches(path, openedFilePath, window);
            NEditFree(path);
            inHunk = 0;
        } else if (parseHunkHeader(line, &oldStart, &oldCount,
                    &newStart, &newCount)) {
            inHunk = fileMatches;
            if (inHunk) {
                if (newStart < 1)
                    newStart = 1;
                appendSourceLinesUntil(&builder, sourceText, &sourcePos,
                        &sourceLine, newStart);
                matchedFile = 1;
            }
            (void)oldStart;
            (void)oldCount;
            (void)newCount;
        } else if (inHunk) {
            if (line[0] == '+' && strncmp(line, "+++", 3)) {
                int start = (int)builder.length;
                appendDiffLine(&builder, line + 1);
                addDiffRange(addRanges, start, (int)builder.length);
                advanceSourceLine(sourceText, &sourcePos);
                sourceLine++;
            } else if (line[0] == '-' && strncmp(line, "---", 3)) {
                int start = (int)builder.length;
                appendDiffLine(&builder, line + 1);
                addDiffRange(delRanges, start, (int)builder.length);
            } else if (line[0] == ' ') {
                appendDisplayedNewLine(&builder, sourceText, &sourcePos,
                        &sourceLine, line + 1, NULL);
            } else if (line[0] == '\\') {
                /* "\ No newline at end of file" marker. */
            } else {
                inHunk = 0;
            }
        }

        line = next;
    }

    NEditFree(copy);
    if (!matchedFile) {
        freeTextBuilder(&builder);
        NEditFree(sourceText);
        return 0;
    }

    appendRemainingSource(&builder, sourceText, &sourcePos);
    *viewText = builder.text;
    NEditFree(sourceText);
    return 1;
}

static int parseHunkHeader(const char *line, int *oldStart, int *oldCount,
        int *newStart, int *newCount)
{
    const char *p = line;

    if (p[0] != '@' || p[1] != '@')
        return 0;
    p += 2;

    if (!parseRange(&p, '-', oldStart, oldCount))
        return 0;
    if (!parseRange(&p, '+', newStart, newCount))
        return 0;

    return 1;
}

static int parseRange(const char **text, char marker, int *start, int *count)
{
    const char *p = skipConstSpace(*text);
    char *endPtr;
    long value;

    if (*p != marker)
        return 0;
    p++;

    value = strtol(p, &endPtr, 10);
    if (endPtr == p)
        return 0;
    *start = (int)value;
    p = endPtr;

    if (*p == ',') {
        p++;
        value = strtol(p, &endPtr, 10);
        if (endPtr == p)
            return 0;
        *count = (int)value;
        p = endPtr;
    } else {
        *count = 1;
    }

    *text = p;
    return 1;
}

static char *copyDiffPath(const char *text)
{
    const char *start = skipConstSpace(text);
    const char *end;
    char *path;
    size_t len;

    if (*start == '"') {
        start++;
        end = start;
        while (*end != '\0' && *end != '"')
            end++;
    } else {
        end = start;
        while (*end != '\0' && !isspace((unsigned char)*end))
            end++;
    }

    len = (size_t)(end - start);
    path = (char *)NEditMalloc(len + 1);
    strncpy(path, start, len);
    path[len] = '\0';

    if (!strncmp(path, "a/", 2) || !strncmp(path, "b/", 2))
        memmove(path, path + 2, strlen(path + 2) + 1);
    while (!strncmp(path, "./", 2))
        memmove(path, path + 2, strlen(path + 2) + 1);

    if (!strcmp(path, "/dev/null")) {
        NEditFree(path);
        return NULL;
    }

    return path;
}

static int pathMatches(const char *diffPath, const char *openedFilePath,
        const WindowInfo *window)
{
    char windowPath[2 * MAXPATHLEN];
    const char *diff;
    const char *opened;

    if (diffPath == NULL || window == NULL)
        return 0;

    diff = skipDotSlash(diffPath);
    opened = openedFilePath == NULL ? "" : skipDotSlash(openedFilePath);

    sprintf(windowPath, "%s%s", window->path, window->filename);

    if (!strcmp(diff, window->filename))
        return 1;
    if (opened[0] != '\0' && !strcmp(diff, opened))
        return 1;
    if (!strcmp(diff, skipDotSlash(windowPath)))
        return 1;
    if (opened[0] != '\0' && pathEndsWith(opened, diff))
        return 1;
    if (pathEndsWith(windowPath, diff))
        return 1;
    if (opened[0] != '\0' && !strcmp(diff, baseName(opened)))
        return 1;

    return 0;
}

static int pathEndsWith(const char *path, const char *suffix)
{
    size_t pathLen;
    size_t suffixLen;

    path = skipDotSlash(path);
    suffix = skipDotSlash(suffix);
    pathLen = strlen(path);
    suffixLen = strlen(suffix);

    if (suffixLen == 0 || pathLen < suffixLen)
        return 0;
    if (!strcmp(path, suffix))
        return 1;
    if (pathLen > suffixLen && path[pathLen - suffixLen - 1] == '/'
            && !strcmp(path + pathLen - suffixLen, suffix))
        return 1;

    return 0;
}

static const char *baseName(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash == NULL ? path : slash + 1;
}

static const char *skipDotSlash(const char *path)
{
    while (path != NULL && path[0] == '.' && path[1] == '/')
        path += 2;
    return path == NULL ? "" : path;
}

static const char *skipConstSpace(const char *text)
{
    while (*text != '\0' && isspace((unsigned char)*text))
        text++;
    return text;
}

static void trimLineEnd(char *line)
{
    size_t len = strlen(line);

    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
        line[len - 1] = '\0';
        len--;
    }
}

static void initRangeList(DiffRangeList *list)
{
    list->ranges = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void freeRangeList(DiffRangeList *list)
{
    if (list->ranges != NULL)
        NEditFree(list->ranges);
    initRangeList(list);
}

static void addDiffRange(DiffRangeList *list, int start, int end)
{
    DiffRange *newRanges;
    int newCapacity;

    if (end <= start)
        return;

    if (list->count == list->capacity) {
        newCapacity = list->capacity == 0 ? 16 : list->capacity * 2;
        newRanges = (DiffRange *)NEditRealloc(list->ranges,
                (size_t)newCapacity * sizeof(DiffRange));
        list->ranges = newRanges;
        list->capacity = newCapacity;
    }

    list->ranges[list->count].start = start;
    list->ranges[list->count].end = end;
    list->count++;
}

static int applyDiffRanges(Rangeset *set, const DiffRangeList *list)
{
    int i;
    int applied = 0;

    if (set == NULL || list == NULL)
        return 0;

    for (i = 0; i < list->count; i++) {
        RangesetAddBetween(set, list->ranges[i].start, list->ranges[i].end);
        applied++;
    }

    return applied;
}

static void initTextBuilder(TextBuilder *builder, size_t initialCapacity)
{
    if (initialCapacity < 1024)
        initialCapacity = 1024;
    builder->text = (char *)NEditMalloc(initialCapacity);
    builder->length = 0;
    builder->capacity = initialCapacity;
    builder->text[0] = '\0';
}

static void freeTextBuilder(TextBuilder *builder)
{
    if (builder->text != NULL)
        NEditFree(builder->text);
    builder->text = NULL;
    builder->length = 0;
    builder->capacity = 0;
}

static void appendBytes(TextBuilder *builder, const char *text, size_t length)
{
    size_t newLength = builder->length + length;

    if (newLength + 1 > builder->capacity) {
        while (newLength + 1 > builder->capacity)
            builder->capacity *= 2;
        builder->text = (char *)NEditRealloc(builder->text,
                builder->capacity);
    }

    if (length > 0)
        memcpy(builder->text + builder->length, text, length);
    builder->length = newLength;
    builder->text[builder->length] = '\0';
}

static void appendDiffLine(TextBuilder *builder, const char *text)
{
    appendBytes(builder, text, strlen(text));
    appendBytes(builder, "\n", 1);
}

static int appendSourceLine(TextBuilder *builder, const char *source,
        size_t *sourcePos)
{
    size_t start;
    size_t end;

    if (source[*sourcePos] == '\0')
        return 0;

    start = *sourcePos;
    end = start;
    while (source[end] != '\0' && source[end] != '\n')
        end++;
    if (source[end] == '\n')
        end++;

    appendBytes(builder, source + start, end - start);
    *sourcePos = end;
    return 1;
}

static void advanceSourceLine(const char *source, size_t *sourcePos)
{
    if (source[*sourcePos] == '\0')
        return;

    while (source[*sourcePos] != '\0' && source[*sourcePos] != '\n')
        (*sourcePos)++;
    if (source[*sourcePos] == '\n')
        (*sourcePos)++;
}

static void appendSourceLinesUntil(TextBuilder *builder, const char *source,
        size_t *sourcePos, int *sourceLine, int targetLine)
{
    if (targetLine < 1)
        targetLine = 1;

    while (*sourceLine < targetLine && source[*sourcePos] != '\0') {
        appendSourceLine(builder, source, sourcePos);
        (*sourceLine)++;
    }
}

static void appendRemainingSource(TextBuilder *builder, const char *source,
        size_t *sourcePos)
{
    appendBytes(builder, source + *sourcePos, strlen(source + *sourcePos));
    *sourcePos += strlen(source + *sourcePos);
}

static void appendDisplayedNewLine(TextBuilder *builder, const char *source,
        size_t *sourcePos, int *sourceLine, const char *fallback,
        DiffRangeList *ranges)
{
    int start = (int)builder->length;

    if (!appendSourceLine(builder, source, sourcePos))
        appendDiffLine(builder, fallback);
    if (ranges != NULL)
        addDiffRange(ranges, start, (int)builder->length);
    (*sourceLine)++;
}

static void clearExistingDiffRanges(RangesetTable *table)
{
    int found;

    if (table == NULL)
        return;

    do {
        unsigned char *labels = RangesetGetList(table);
        int i;

        found = 0;
        for (i = 0; labels[i] != '\0'; i++) {
            Rangeset *set = RangesetFetch(table, labels[i]);
            char *name = set == NULL ? NULL : RangesetGetName(set);

            if (name != NULL && (!strcmp(name, "diff-additions") ||
                        !strcmp(name, "diff-deletions"))) {
                RangesetForget(table, labels[i]);
                found = 1;
                break;
            }
        }
    } while (found);
}
