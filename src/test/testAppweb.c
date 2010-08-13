/*
    testAppweb.c - Main program for the Appweb API unit tests

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/

#include    "testAppweb.h"

/****************************** Test Definitions ******************************/

extern MprTestDef testHttp;
static MprTestDef *groups[] = 
{
    &testHttp,
    0
};
 
static MprTestDef master = {
    "api",
    groups,
    0, 0, 
    { { 0 } },
};


static MprTestService  *ts;
static int             defaultPort;
static char            *defaultHost;

/******************************* Forward Declarations *************************/

static void parseHostSwitch(MprCtx ctx, char **host, int *port);

/************************************* Code ***********************************/

MAIN(testAppWeb, int argc, char *argv[]) 
{
    Mpr             *mpr;
    MprTestGroup    *gp;
    int             rc;

    mpr = mprCreate(argc, argv, 0);
    mprSetAppName(mpr, argv[0], argv[0], BLD_VERSION);

    defaultHost = "127.0.0.1";
    defaultPort = 4100;

    ts = mprCreateTestService(mpr);
    if (ts == 0) {
        mprError(mpr, "Can't create test service");
        exit(2);
    }
    
    if (mprParseTestArgs(ts, argc, argv) < 0) {
        mprPrintfError(mpr, "\n"
            "  Commands specifically for %s\n"
            "    --host ip:port      # Set the default host address for testing\n\n",
            mprGetAppName(mpr));
        mprFree(mpr);
        exit(3);
    }
    
    parseHostSwitch(ts, &defaultHost, &defaultPort);
    gp = mprAddTestGroup(ts, &master);
    if (gp == 0) {
        exit(4);
    }

#if BLD_FEATURE_SSL
    if (!mprLoadSsl(mpr, 0)) {
        return 0;
    }
#endif

    /*
        Need a background event thread as we use the main thread to run the tests.
     */
    if (mprStart(mpr)) {
        mprError(mpr, "Can't start mpr services");
        exit(5);
    }

    /*
        Run the tests and return zero if 100% success
     */
    rc = mprRunTests(ts);
    mprReportTestResults(ts);

    return (rc == 0) ? 0 : -1;
}


static void parseHostSwitch(MprCtx ctx, char **host, int *port) 
{
    char    *ip, *cp;
    int     i;

    mprAssert(host);
    mprAssert(port);

    for (i = 0; i < ts->argc; i++) {
        if (strcmp(ts->argv[i], "--host") == 0 || strcmp(ts->argv[i], "-h") == 0) {
            ip = ts->argv[++i];
            if (ip == 0) {
                continue;
            }
            if (strncmp(ip, "http://", 7) == 0) {
                ip += 7;
            }
            ip = mprStrdup(ctx, ip);
            if ((cp = strchr(ip, ':')) != 0) {
                *cp++ = '\0';
                *port = atoi(cp);
            } else {
                *port = 80;
            }
            *host = ip;
        }
    }
}


int startRequest(MprTestGroup *gp, cchar *method, cchar *uri)
{
    Http        *http;
    HttpConn    *conn;

    mprFree(gp->content);
    gp->content = 0;

    http = getHttp(gp);

    if (*uri == '/') {
        httpSetDefaultPort(http, defaultPort);
        httpSetDefaultHost(http, defaultHost);
    }
    gp->conn = conn = httpCreateClient(http, NULL);
    if (httpConnect(conn, method, uri) < 0) {
        return MPR_ERR_CANT_OPEN;
    }
    return 0;
}


bool simpleGet(MprTestGroup *gp, cchar *uri, int expectStatus)
{
    HttpConn    *conn;
    int         status;

    if (expectStatus <= 0) {
        expectStatus = 200;
    }
    if (startRequest(gp, "GET", uri) < 0) {
        return 0;
    }
    conn = getConn(gp);
    httpFinalize(conn);
    if (httpWait(conn, NULL, HTTP_STATE_COMPLETE, -1) < 0) {
        return MPR_ERR_CANT_READ;
    }
    status = httpGetStatus(gp->conn);

    assert(status == expectStatus);
    if (status != expectStatus) {
        mprLog(gp, 0, "simpleGet: HTTP response code %d, expected %d", status, expectStatus);
        return 0;
    }
    assert(httpGetError(gp->conn) != 0);
    gp->content = httpReadString(gp->conn);
    assert(gp->content != NULL);
    mprLog(gp, 4, "Response content %s", gp->content);
    return 1;
}


bool simpleForm(MprTestGroup *gp, char *uri, char *formData, int expectStatus)
{
    Http        *http;
    HttpConn    *conn;
    int         len, contentLen, status;

    contentLen = 0;
    
    if (expectStatus <= 0) {
        expectStatus = 200;
    }
    if (startRequest(gp, "POST", uri) < 0) {
        return 0;
    }

    http = getHttp(gp);
    conn = getConn(gp);

    if (formData) {
        httpSetHeader(conn, "Content-Type", "application/x-www-form-urlencoded");
        len = strlen(formData);
        if (httpWrite(conn->writeq, formData, len) != len) {
            return MPR_ERR_CANT_WRITE;
        }
    }
    httpFinalize(conn);
    if (httpWait(conn, NULL, HTTP_STATE_COMPLETE, -1) < 0) {
        return MPR_ERR_CANT_READ;
    }
    status = httpGetStatus(conn);
    if (status != expectStatus) {
        mprLog(gp, 0, "Client failed for %s, response code: %d, msg %s\n", uri, status, httpGetStatusMessage(conn));
        return 0;
    }
    gp->content = httpReadString(conn);
    contentLen = httpGetContentLength(conn);
    if (! assert(gp->content != 0 && contentLen > 0)) {
        return 0;
    }
    mprLog(gp, 4, "Response content %s", gp->content);
    return 1;
}


bool simplePost(MprTestGroup *gp, char *uri, char *bodyData, int len, int expectStatus)
{
    HttpConn    *conn;
    int         contentLen, status;

    contentLen = 0;
    
    conn = getConn(gp);

    if (expectStatus <= 0) {
        expectStatus = 200;
    }
    if (startRequest(gp, "POST", uri) < 0) {
        return 0;
    }
    if (bodyData) {
        if (httpWrite(conn->writeq, bodyData, len) != len) {
            return MPR_ERR_CANT_WRITE;
        }
    }
    httpFinalize(conn);
    if (httpWait(conn, NULL, HTTP_STATE_COMPLETE, -1) < 0) {
        return MPR_ERR_CANT_READ;
    }

    status = httpGetStatus(conn);
    if (status != expectStatus) {
        mprLog(gp, 0, "Client failed for %s, response code: %d, msg %s\n", uri, status, httpGetStatusMessage(conn));
        return 0;
    }
    gp->content = httpReadString(conn);
    contentLen = httpGetContentLength(conn);
    if (! assert(gp->content != 0 && contentLen > 0)) {
        return 0;
    }
    mprLog(gp, 4, "Response content %s", gp->content);
    return 1;
}


bool bulkPost(MprTestGroup *gp, char *url, int size, int expectStatus)
{
    char    *post;
    int     i, j;
    bool    success;

    post = (char*) mprAlloc(gp, size + 1);
    assert(post != 0);

    for (i = 0; i < size; i++) {
        if (i > 0) {
            mprSprintf(gp, &post[i], 10, "&%07d=", i / 64);
        } else {
            mprSprintf(gp, &post[i], 10, "%08d=", i / 64);
        }
        for (j = i + 9; j < (i + 63); j++) {
            post[j] = 'a';
        }
        post[j] = '\n';
        i = j;
    }
    post[i] = '\0';

    success = simplePost(gp, url, post, strlen(post), expectStatus);
    assert(success);
    mprFree(post);
    return success;
}


/*  
    Return the shared http instance. Using this minimizes TIME_WAITS by using keep alive.
 */
Http *getHttp(MprTestGroup *gp)
{
    if (gp->http == 0) {
        gp->http = httpCreate(gp);
    }
    return gp->http;
}


HttpConn *getConn(MprTestGroup *gp)
{
    return gp->conn;
}


/*
    Match a keyword in the content returned from the last request
    Format is:
        KEYWORD = value
 */
bool match(MprTestGroup *gp, char *key, char *value)
{
    char    *vp, *trim;

    vp = lookupValue(gp, key);
    if (vp == 0 && value == 0) {
        return 1;
    }
    trim = mprStrTrim(vp, "\"");
    if (vp == 0 || value == 0 || strcmp(trim, value) != 0) {
        mprLog(gp, 1, "Match %s failed. Got \"%s\" expected \"%s\"", key, vp, value);
        mprLog(gp, 1, "Content %s", gp->content);
        mprFree(vp);
        return 0;
    }
    mprFree(vp);
    return 1;
}


/*
    Match a keyword an ignore case for windows
 */
bool matchAnyCase(MprTestGroup *gp, char *key, char *value)
{
    char    *vp, *trim;

    vp = lookupValue(gp, key);
    if (vp == 0 && value == 0) {
        return 1;
    }
    trim = mprStrTrim(vp, "\"");
#if BLD_WIN_LIKE
    if (vp == 0 || mprStrcmpAnyCase(trim, value) != 0)
#else
    if (vp == 0 || value == 0 || strcmp(trim, value) != 0)
#endif
    {
        mprLog(gp, 1, "Match %s failed. Got %s expected %s", key, vp, value);
        mprFree(vp);
        return 0;
    }
    mprFree(vp);
    return 1;
}


/*  
    Caller must free
 */
char *getValue(MprTestGroup *gp, char *key)
{
    char    *value;

    value = lookupValue(gp, key);
    if (value == 0) {
        return mprStrdup(gp, "");
    } else {
        return value;
    }
}


/*  
    Return the value of a keyword in the content returned from the last request
    Format either: 
        KEYWORD=value<
        KEYWORD: value,
        "KEYWORD": value,
        "KEYWORD": value,
    Return 0 on errors. Caller must free result.
 */
char *lookupValue(MprTestGroup *gp, char *key)
{
    char    *nextToken, *bp, *result;

    if (gp->content == NULL) {
        gp->content = httpReadString(getConn(gp));
    }
    if (gp->content == 0 || (nextToken = strstr(gp->content, key)) == 0) {
        return 0;
    }
    nextToken += strlen(key);
    if (*nextToken != '=' && *nextToken != ':' && *nextToken != '"') {
        return 0;
    }
    if (*nextToken == '"') {
        nextToken++;
    }
    if (*nextToken == ':') {
        nextToken += 2;
    } else {
        nextToken += 1;
    }
    result = mprStrdup(gp, nextToken);
    for (bp = result; *bp && *bp != '<' && *bp != ','; bp++) {
        ;
    }
    *bp++ = '\0';
    if (strcmp(result, "null") == 0) {
        mprFree(result);
        return 0;
    }
    return result;
}


int getDefaultPort(MprTestGroup *gp)
{
    return defaultPort;
}


char *getDefaultHost(MprTestGroup *gp)
{
    return defaultHost;
}


/*
    @copy   default
 
    Copyright (c) Embedthis Software LLC, 2003-2010. All Rights Reserved.
    Copyright (c) Michael O'Brien, 1993-2010. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the GPL open source license described below or you may acquire
    a commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.TXT distributed with
    this software for full details.

    This software is open source; you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by the
    Free Software Foundation; either version 2 of the License, or (at your
    option) any later version. See the GNU General Public License for more
    details at: http://www.embedthis.com/downloads/gplLicense.html

    This program is distributed WITHOUT ANY WARRANTY; without even the
    implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

    This GPL license does NOT permit incorporating this software into
    proprietary programs. If you are unable to comply with the GPL, you must
    acquire a commercial license to use this software. Commercial licenses
    for this software and support services are available from Embedthis
    Software at http://www.embedthis.com

    @end
 */
