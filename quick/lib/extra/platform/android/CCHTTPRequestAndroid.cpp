#include "network/CCHTTPRequest.h"
#include <stdio.h>
#include <iostream>
#include <thread>
#include "platform/android/jni/jniHelper.h"

#if CC_LUA_ENGINE_ENABLED > 0
extern "C" {
#include "lua.h"
}
#include "CCLuaEngine.h"
#endif
#include <sstream>


NS_CC_EXTRA_BEGIN

unsigned int HTTPRequest::s_id = 0;

// jni helper function
jobject getInstance(JNIEnv* env, jclass obj_class)
{  
    jmethodID construction_id = env->GetMethodID(obj_class, "<init>", "()V");  
    jobject obj = env->NewObject(obj_class, construction_id);  
    return obj;  
} 

HTTPRequest *HTTPRequest::createWithUrl(HTTPRequestDelegate *delegate,
                                            const char *url,
                                            int method)
{
    HTTPRequest *request = new HTTPRequest();
    request->initWithDelegate(delegate, url, method);
    request->autorelease();
    return request;
}

#if CC_LUA_ENGINE_ENABLED > 0
HTTPRequest *HTTPRequest::createWithUrlLua(LUA_FUNCTION listener,
                                               const char *url,
                                               int method)
{
    HTTPRequest *request = new HTTPRequest();
    request->initWithListener(listener, url, method);
    request->autorelease();
    return request;
}
#endif

bool HTTPRequest::initWithDelegate(HTTPRequestDelegate *delegate, const char *url, int method)
{
    m_delegate = delegate;
    return initWithUrl(url, method);
}

#if CC_LUA_ENGINE_ENABLED > 0
bool HTTPRequest::initWithListener(LUA_FUNCTION listener, const char *url, int method)
{
    m_listener = listener;
    return initWithUrl(url, method);
}
#endif

bool HTTPRequest::initWithUrl(const char *url, int method)
{
    CCAssert(url, "HTTPRequest::initWithUrl() - invalid url");

    m_url = url;

    if (method == kCCHTTPRequestMethodPOST) {
        m_httpMethod = "POST";
    } else {
        m_httpMethod = "GET";
    }

    ++s_id;
    // CCLOG("HTTPRequest[0x%04x] - create request with url: %s", s_id, url);
    return true;
}

HTTPRequest::~HTTPRequest(void)
{
    cleanup();
    if (m_listener)
    {
        LuaEngine::getInstance()->removeScriptHandler(m_listener);
    }
    // CCLOG("HTTPRequest[0x%04x] - request removed", s_id);
}

void HTTPRequest::setRequestUrl(const char *url)
{
    CCAssert(url, "HTTPRequest::setRequestUrl() - invalid url");
    m_url = url;
}

const string HTTPRequest::getRequestUrl(void)
{
    return m_url;
}

void HTTPRequest::addRequestHeader(const char *header)
{
    CCAssert(m_state == kCCHTTPRequestStateIdle, "HTTPRequest::addRequestHeader() - request not idle");
    CCAssert(header, "HTTPRequest::addRequestHeader() - invalid header");
    m_headers.push_back(string(header));
}

void HTTPRequest::addPOSTValue(const char *key, const char *value)
{
    CCAssert(m_state == kCCHTTPRequestStateIdle, "HTTPRequest::addPOSTValue() - request not idle");
    CCAssert(key, "HTTPRequest::addPOSTValue() - invalid key");
    m_postFields[string(key)] = string(value ? value : "");
}

void HTTPRequest::setPOSTData(const char *data)
{
    CCAssert(m_state == kCCHTTPRequestStateIdle, "HTTPRequest::setPOSTData() - request not idle");
    CCAssert(data, "HTTPRequest::setPOSTData() - invalid post data");
    m_postFields.clear();
}

void HTTPRequest::addFormFile(const char *name, const char *filePath, const char *contentType)
{
    m_postFile[string(name)] = string(filePath);
}

void HTTPRequest::addFormContents(const char *name, const char *value)
{
    m_postContent[string(name)] = string(value);
}

void HTTPRequest::setCookieString(const char *cookie)
{
    CCAssert(m_state == kCCHTTPRequestStateIdle, "HTTPRequest::setAcceptEncoding() - request not idle");
    m_cookies = cookie ? cookie : "";
}

const string HTTPRequest::getCookieString(void)
{
    CCAssert(m_state == kCCHTTPRequestStateCompleted, "HTTPRequest::getResponseData() - request not completed");
    return m_responseCookies;
}

void HTTPRequest::setAcceptEncoding(int acceptEncoding)
{
    CCAssert(m_state == kCCHTTPRequestStateIdle, "HTTPRequest::setAcceptEncoding() - request not idle");
    //TODO
}

void HTTPRequest::setTimeout(int timeout)
{
    CCAssert(m_state == kCCHTTPRequestStateIdle, "HTTPRequest::setTimeout() - request not idle");
    m_nTimeOut = timeout;
}

bool HTTPRequest::start(void)
{
    CCAssert(m_state == kCCHTTPRequestStateIdle, "HTTPRequest::start() - request not idle");

    m_state = kCCHTTPRequestStateInProgress;
    m_curlState = kCCHTTPRequestCURLStateBusy;
    retain();

    createURLConnectJava();
    setRequestMethodJava();
    setTimeoutJava(m_nTimeOut);

    for (HTTPRequestHeadersIterator it = m_headers.begin(); it != m_headers.end(); ++it)
    {
        string val = *it;
        int len = val.length();
        int pos = val.find('=');
        if (-1 == pos || pos >= len) {
            continue;
        }
        string str1 = val.substr(0, pos);
        string str2 = val.substr(pos + 1, len - pos - 1);

        addRequestHeaderJava(str1.c_str(), str2.c_str());
    }

    if (m_cookies && 0 != strlen(m_cookies)) {
        addRequestHeaderJava("Cookie", m_cookies);
    }

    // memset(&m_thread, 0, sizeof(pthread_t));
    // memset(&m_threadAttr, 0, sizeof(pthread_attr_t));
    // pthread_attr_init (&m_threadAttr);
    // pthread_attr_setdetachstate (&m_threadAttr,PTHREAD_CREATE_DETACHED);
    // pthread_create(&m_thread, &m_threadAttr, requestCURL, this);
    pthread_create(&m_thread, NULL, requestCURL, this);
    // pthread_detach(m_thread);
    
    Director::getInstance()->getScheduler()->scheduleUpdateForTarget(this, 0, false);
    // CCLOG("HTTPRequest[0x%04x] - request start", s_id);
    return true;
}

void HTTPRequest::cancel(void)
{
    m_delegate = NULL;
    if (m_state == kCCHTTPRequestStateIdle || m_state == kCCHTTPRequestStateInProgress)
    {
        m_state = kCCHTTPRequestStateCancelled;
    }
}

int HTTPRequest::getState(void)
{
    return m_state;
}

int HTTPRequest::getResponseStatusCode(void)
{
    CCAssert(m_state == kCCHTTPRequestStateCompleted, "Request not completed");
    return m_responseCode;
}

const HTTPRequestHeaders &HTTPRequest::getResponseHeaders(void)
{
    CCAssert(m_state == kCCHTTPRequestStateCompleted, "HTTPRequest::getResponseHeaders() - request not completed");
    return m_responseHeaders;
}

const string HTTPRequest::getResponseHeadersString()
{
    string buf;
    for (HTTPRequestHeadersIterator it = m_responseHeaders.begin(); it != m_responseHeaders.end(); ++it)
    {
        buf.append(*it);
    }
    return buf;
}

const string HTTPRequest::getResponseString(void)
{
    CCAssert(m_state == kCCHTTPRequestStateCompleted, "HTTPRequest::getResponseString() - request not completed");
    return string(m_responseBuffer ? static_cast<char*>(m_responseBuffer) : "");
}

void *HTTPRequest::getResponseData(void)
{
    CCAssert(m_state == kCCHTTPRequestStateCompleted, "HTTPRequest::getResponseData() - request not completed");
    void *buff = malloc(m_responseDataLength);
    memcpy(buff, m_responseBuffer, m_responseDataLength);
    return buff;
}

#if CC_LUA_ENGINE_ENABLED > 0
LUA_STRING HTTPRequest::getResponseDataLua(void)
{
    CCAssert(m_state == kCCHTTPRequestStateCompleted, "HTTPRequest::getResponseDataLua() - request not completed");
    LuaStack *stack = LuaEngine::getInstance()->getLuaStack();
    stack->clean();
    stack->pushString(static_cast<char*>(m_responseBuffer), (int)m_responseDataLength);
    return 1;
}
#endif

int HTTPRequest::getResponseDataLength(void)
{
    CCAssert(m_state == kCCHTTPRequestStateCompleted, "Request not completed");
    return (int)m_responseDataLength;
}

size_t HTTPRequest::saveResponseData(const char *filename)
{
    CCAssert(m_state == kCCHTTPRequestStateCompleted, "HTTPRequest::saveResponseData() - request not completed");
    
    FILE *fp = fopen(filename, "wb");
    CCAssert(fp, "HTTPRequest::saveResponseData() - open file failure");
    
    size_t writedBytes = m_responseDataLength;
    if (writedBytes > 0)
    {
        fwrite(m_responseBuffer, m_responseDataLength, 1, fp);
    }
    fclose(fp);
    return writedBytes;
}

int HTTPRequest::getErrorCode(void)
{
    return m_errorCode;
}

const string HTTPRequest::getErrorMessage(void)
{
    return m_errorMessage;
}

HTTPRequestDelegate* HTTPRequest::getDelegate(void)
{
    return m_delegate;
}

void HTTPRequest::checkCURLState(float dt)
{
    CC_UNUSED_PARAM(dt);
    if (m_curlState != kCCHTTPRequestCURLStateBusy)
    {
        Director::getInstance()->getScheduler()->unscheduleAllForTarget(this);
        release();
    }
}

void HTTPRequest::update(float dt)
{
    if (m_state == kCCHTTPRequestStateInProgress) {
        

        return;
    }
    Director::getInstance()->getScheduler()->unscheduleAllForTarget(this);
    if (m_curlState != kCCHTTPRequestCURLStateIdle)
    {
        Director::getInstance()->getScheduler()->scheduleSelector(schedule_selector(HTTPRequest::checkCURLState), this, 0, false);
    }

    if (m_state == kCCHTTPRequestStateCompleted)
    {
        // CCLOG("HTTPRequest[0x%04x] - request completed", s_id);
        if (m_delegate) m_delegate->requestFinished(this);
    }
    else
    {
        // CCLOG("HTTPRequest[0x%04x] - request failed", s_id);
        if (m_delegate) m_delegate->requestFailed(this);
    }

#if CC_LUA_ENGINE_ENABLED > 0
    if (m_listener)
    {
        LuaValueDict dict;

        switch (m_state)
        {
            case kCCHTTPRequestStateCompleted:
                dict["name"] = LuaValue::stringValue("completed");
                break;
                
            case kCCHTTPRequestStateCancelled:
                dict["name"] = LuaValue::stringValue("cancelled");
                break;
                
            case kCCHTTPRequestStateFailed:
                dict["name"] = LuaValue::stringValue("failed");
                break;
                
            default:
                dict["name"] = LuaValue::stringValue("unknown");
        }
        dict["request"] = LuaValue::ccobjectValue(this, "HTTPRequest");
        LuaStack *stack = LuaEngine::getInstance()->getLuaStack();
        stack->clean();
        stack->pushLuaValueDict(dict);
        stack->executeFunctionByHandler(m_listener, 1);
    }
#endif
}

// instance callback

void HTTPRequest::onRequest(void)
{
    int nSuc = connectJava();
    int code = 0;

    if (0 == nSuc) {
        if (m_postFields.size() > 0)
        {
            for (Fields::iterator it = m_postFields.begin(); it != m_postFields.end(); ++it)
            {
                postContentJava(it->first.c_str(), it->second.c_str());
            }
        }

        if (m_postContent.size() > 0)
        {
            for (Fields::iterator it = m_postContent.begin(); it != m_postContent.end(); ++it)
            {
                postContentJava(it->first.c_str(), it->second.c_str());
            }
        }
        if (m_postFile.size() > 0)
        {
            for (Fields::iterator it = m_postFile.begin(); it != m_postFile.end(); ++it)
            {
                postFileJava(it->first.c_str(), it->second.c_str());
            }
        }

        //set cookie TODO

        code = getResponedCodeJava();

        CCLOG("HTTPRequest responed code:%d", code);

        char* header = NULL;

        int nCounter = 0;
        while(true) {
            header = getResponedHeaderByIdxJava(nCounter);
            if (NULL == header) {
                break;
            }
            if (0 == strcasecmp("Set-Cookie", header)) {
            } else {
                onWriteHeader(header, strlen(header));
            }
            nCounter++;
        }

        //get cookies
        CCLOG("get cookies");
        m_responseCookies = getResponedHeaderByKeyJava("set-cookie");

        CCLOG("delete succesdddddds");

        while (true) {
            char* recvData = getResponedStringJava();
            if (NULL == recvData) {
                code = 0;
                CCLOG("HTTPRequest - onRequest, get null responed string");
                break;
            } else {
                int nRecv = strlen(recvData);
                if (1 == (char)(*recvData)) {
                    nRecv -= 1;
                    CCLOG("HTTPRequest - onRequest, responed string len:%d", nRecv);
                    onWriteData(recvData + 1, nRecv);
                    onProgress(m_responseDataLength + nRecv, nRecv, 0, 0);
                } else {
                    CCLOG("HTTPRequest - onRequest, responed string completed:%d", nRecv);
                    break;
                }
            }
        }
    }

    m_errorCode = code;
    m_responseCode = code;
    m_errorMessage = (code == 200) ? "" : getResponedErrJava();
    m_state = (code == 200) ? kCCHTTPRequestStateCompleted : kCCHTTPRequestStateFailed;
    m_curlState = kCCHTTPRequestCURLStateClosed;
}

size_t HTTPRequest::onWriteData(void *buffer, size_t bytes)
{
    if (m_responseDataLength + bytes + 1 > m_responseBufferLength)
    {
        m_responseBufferLength += BUFFER_CHUNK_SIZE;
        m_responseBuffer = realloc(m_responseBuffer, m_responseBufferLength);
    }

    memcpy(static_cast<char*>(m_responseBuffer) + m_responseDataLength, buffer, bytes);
    m_responseDataLength += bytes;
    static_cast<char*>(m_responseBuffer)[m_responseDataLength] = 0;
    return bytes;
}

size_t HTTPRequest::onWriteHeader(void *buffer, size_t bytes)
{
    char *headerBuffer = new char[bytes + 1];
    headerBuffer[bytes] = 0;
    memcpy(headerBuffer, buffer, bytes);    
    m_responseHeaders.push_back(string(headerBuffer));
    delete []headerBuffer;
    return bytes;
}

int HTTPRequest::onProgress(double dltotal, double dlnow, double ultotal, double ulnow)
{
    return m_state == kCCHTTPRequestStateCancelled ? 1: 0;
}

void HTTPRequest::cleanup(void)
{
    m_state = kCCHTTPRequestStateCleared;
    m_responseBufferLength = 0;
    m_responseDataLength = 0;
    if (m_responseBuffer)
    {
        free(m_responseBuffer);
        m_responseBuffer = NULL;
    }
    if (m_httpConnect)
    {
        closeJava();
        m_httpConnect = NULL;
    }
}

// curl callback

void *HTTPRequest::requestCURL(void *userdata)
{
    static_cast<HTTPRequest*>(userdata)->onRequest();

    //Detach线程
    JavaVM* jvm = JniHelper::getJavaVM();
    if(jvm->DetachCurrentThread() != JNI_OK) {
        CCLOG("HTTPRequest - requestCURL DetachCurrentThread fail");
    }
    // pthread_detach(pthread_self());
    pthread_exit((void *)0);
    return NULL;
}

size_t HTTPRequest::writeDataCURL(void *buffer, size_t size, size_t nmemb, void *userdata)
{
    return static_cast<HTTPRequest*>(userdata)->onWriteData(buffer, size *nmemb);
}

size_t HTTPRequest::writeHeaderCURL(void *buffer, size_t size, size_t nmemb, void *userdata)
{
    return static_cast<HTTPRequest*>(userdata)->onWriteHeader(buffer, size *nmemb);
}

int HTTPRequest::progressCURL(void *userdata, double dltotal, double dlnow, double ultotal, double ulnow)
{
    return static_cast<HTTPRequest*>(userdata)->onProgress(dltotal, dlnow, ultotal, ulnow);
}



void HTTPRequest::createURLConnectJava() {
    JniMethodInfo methodInfo;
    if (JniHelper::getStaticMethodInfo(methodInfo,
        "org/cocos2dx/lib/QuickHTTPInterface",
        "createURLConnect",
        "(Ljava/lang/String;)Ljava/net/HttpURLConnection;"))
    {
        jstring jurl = methodInfo.env->NewStringUTF(m_url.c_str());
        m_httpConnect = methodInfo.env->CallStaticObjectMethod(methodInfo.classID, methodInfo.methodID, jurl);
        methodInfo.env->DeleteLocalRef(jurl);
        methodInfo.env->DeleteLocalRef(methodInfo.classID);
        m_jniEnv = methodInfo.env;
    }
}

void HTTPRequest::setRequestMethodJava() {
    JniMethodInfo methodInfo;
    if (JniHelper::getStaticMethodInfo(methodInfo,
        "org/cocos2dx/lib/QuickHTTPInterface",
        "setRequestMethod",
        "(Ljava/net/HttpURLConnection;Ljava/lang/String;)V"))
    {
        jstring jstr = methodInfo.env->NewStringUTF(m_httpMethod);
        methodInfo.env->CallStaticVoidMethod(
            methodInfo.classID, methodInfo.methodID, m_httpConnect, jstr);
        methodInfo.env->DeleteLocalRef(jstr);
        methodInfo.env->DeleteLocalRef(methodInfo.classID);
    }
}

void HTTPRequest::addRequestHeaderJava(const char* key, const char* value) {
    JniMethodInfo methodInfo;
    if (JniHelper::getStaticMethodInfo(methodInfo,
        "org/cocos2dx/lib/QuickHTTPInterface",
        "addRequestHeader",
        "(Ljava/net/HttpURLConnection;Ljava/lang/String;Ljava/lang/String;)V"))
    {
        jstring jstrKey = methodInfo.env->NewStringUTF(key);
        jstring jstrVal = methodInfo.env->NewStringUTF(value);
        methodInfo.env->CallStaticVoidMethod(
            methodInfo.classID, methodInfo.methodID, m_httpConnect, jstrKey, jstrVal);
        methodInfo.env->DeleteLocalRef(jstrKey);
        methodInfo.env->DeleteLocalRef(jstrVal);
        methodInfo.env->DeleteLocalRef(methodInfo.classID);
    }
}

void HTTPRequest::setTimeoutJava(int msTime) {
    JniMethodInfo methodInfo;
    if (JniHelper::getStaticMethodInfo(methodInfo,
        "org/cocos2dx/lib/QuickHTTPInterface",
        "setTimeout",
        "(Ljava/net/HttpURLConnection;I)V"))
    {
        methodInfo.env->CallStaticVoidMethod(
            methodInfo.classID, methodInfo.methodID, m_httpConnect, msTime);
        methodInfo.env->DeleteLocalRef(methodInfo.classID);
    }
}

int HTTPRequest::connectJava() {
    int nSuc = 0;
    JniMethodInfo methodInfo;
    if (JniHelper::getStaticMethodInfo(methodInfo,
        "org/cocos2dx/lib/QuickHTTPInterface",
        "connect",
        "(Ljava/net/HttpURLConnection;)I"))
    {
        nSuc = methodInfo.env->CallStaticIntMethod(
            methodInfo.classID, methodInfo.methodID, m_httpConnect);
        methodInfo.env->DeleteLocalRef(methodInfo.classID);
    }

    return nSuc;
}

void HTTPRequest::postContentJava(const char* key, const char* value) {
    JniMethodInfo methodInfo;
    if (JniHelper::getStaticMethodInfo(methodInfo,
        "org/cocos2dx/lib/QuickHTTPInterface",
        "postContent",
        "(Ljava/net/HttpURLConnection;Ljava/lang/String;Ljava/lang/String;)V"))
    {
        jstring jstrKey = methodInfo.env->NewStringUTF(key);
        jstring jstrVal = methodInfo.env->NewStringUTF(value);
        methodInfo.env->CallStaticVoidMethod(
            methodInfo.classID, methodInfo.methodID, m_httpConnect, jstrKey, jstrVal);
        methodInfo.env->DeleteLocalRef(jstrKey);
        methodInfo.env->DeleteLocalRef(jstrVal);
        methodInfo.env->DeleteLocalRef(methodInfo.classID);
    }
}

void HTTPRequest::postFileJava(const char* fileName, const char* filePath) {
    JniMethodInfo methodInfo;
    if (JniHelper::getStaticMethodInfo(methodInfo,
        "org/cocos2dx/lib/QuickHTTPInterface",
        "postFile",
        "(Ljava/net/HttpURLConnection;Ljava/lang/String;Ljava/lang/String;)V"))
    {
        jstring jstrFile = methodInfo.env->NewStringUTF(fileName);
        jstring jstrPath = methodInfo.env->NewStringUTF(filePath);
        methodInfo.env->CallStaticVoidMethod(
            methodInfo.classID, methodInfo.methodID, m_httpConnect, jstrFile, jstrPath);
        methodInfo.env->DeleteLocalRef(jstrFile);
        methodInfo.env->DeleteLocalRef(jstrPath);
        methodInfo.env->DeleteLocalRef(methodInfo.classID);
    }
}

int HTTPRequest::getResponedCodeJava() {
    int nResponed = 0;
    JniMethodInfo methodInfo;
    if (JniHelper::getStaticMethodInfo(methodInfo,
        "org/cocos2dx/lib/QuickHTTPInterface",
        "getResponedCode",
        "(Ljava/net/HttpURLConnection;)I"))
    {
        nResponed = methodInfo.env->CallStaticIntMethod(
            methodInfo.classID, methodInfo.methodID, m_httpConnect);
        methodInfo.env->DeleteLocalRef(methodInfo.classID);
    }

    return nResponed;
}

char* HTTPRequest::getResponedErrJava() {
    char* error = nullptr;
    JniMethodInfo methodInfo;
    if (JniHelper::getStaticMethodInfo(methodInfo,
        "org/cocos2dx/lib/QuickHTTPInterface",
        "getResponedErr",
        "(Ljava/net/HttpURLConnection;)Ljava/lang/String;"))
    {
        jobject jObj = methodInfo.env->CallStaticObjectMethod(
            methodInfo.classID, methodInfo.methodID, m_httpConnect);
        error = getCStrFromJString((jstring)jObj, methodInfo.env);
        if (NULL != jObj) {
            methodInfo.env->DeleteLocalRef(jObj);
        }
        methodInfo.env->DeleteLocalRef(methodInfo.classID);
    }

    return error;
}

char* HTTPRequest::getResponedHeaderJava() {
    char* header = nullptr;
    JniMethodInfo methodInfo;
    if (JniHelper::getStaticMethodInfo(methodInfo,
        "org/cocos2dx/lib/QuickHTTPInterface",
        "getResponedHeader",
        "(Ljava/net/HttpURLConnection;)Ljava/lang/String;"))
    {
        jobject jObj = methodInfo.env->CallStaticObjectMethod(
            methodInfo.classID, methodInfo.methodID, m_httpConnect);
        header = getCStrFromJString((jstring)jObj, methodInfo.env);
        if (NULL != jObj) {
            methodInfo.env->DeleteLocalRef(jObj);
        }
        methodInfo.env->DeleteLocalRef(methodInfo.classID);
    }

    return header;
}

char* HTTPRequest::getResponedHeaderByIdxJava(int idx) {
    char* header = nullptr;
    JniMethodInfo methodInfo;
    if (JniHelper::getStaticMethodInfo(methodInfo,
        "org/cocos2dx/lib/QuickHTTPInterface",
        "getResponedHeaderByIdx",
        "(Ljava/net/HttpURLConnection;I)Ljava/lang/String;"))
    {
        jobject jObj = methodInfo.env->CallStaticObjectMethod(
            methodInfo.classID, methodInfo.methodID, m_httpConnect, idx);
        header = getCStrFromJString((jstring)jObj, methodInfo.env);
        if (NULL != jObj) {
            methodInfo.env->DeleteLocalRef(jObj);
        }
        methodInfo.env->DeleteLocalRef(methodInfo.classID);
    }

    return header;
}

char* HTTPRequest::getResponedHeaderByKeyJava(const char* key) {
    char* value = nullptr;
    JniMethodInfo methodInfo;
    if (JniHelper::getStaticMethodInfo(methodInfo,
        "org/cocos2dx/lib/QuickHTTPInterface",
        "getResponedHeaderByKey",
        "(Ljava/net/HttpURLConnection;Ljava/lang/String;)Ljava/lang/String;"))
    {
        jstring jstrKey = methodInfo.env->NewStringUTF(key);
        jobject jObj = methodInfo.env->CallStaticObjectMethod(
            methodInfo.classID, methodInfo.methodID, m_httpConnect, jstrKey);
        value = getCStrFromJString((jstring)jObj, methodInfo.env);
        methodInfo.env->DeleteLocalRef(jstrKey);
        CCLOG("headerByKey %x", jObj);
        if (NULL != jObj) {
            methodInfo.env->DeleteLocalRef(jObj);
        }
        CCLOG("delete success");
        methodInfo.env->DeleteLocalRef(methodInfo.classID);
        CCLOG("delete succesddddfff2s");
    }

    return value;
}

char* HTTPRequest::getResponedStringJava() {
    char* responed = nullptr;
    JniMethodInfo methodInfo;
    if (JniHelper::getStaticMethodInfo(methodInfo,
        "org/cocos2dx/lib/QuickHTTPInterface",
        "getResponedString",
        "(Ljava/net/HttpURLConnection;)Ljava/lang/String;"))
    {
        jobject jObj = methodInfo.env->CallStaticObjectMethod(
            methodInfo.classID, methodInfo.methodID, m_httpConnect);
        responed = getCStrFromJString((jstring)jObj, methodInfo.env);
        if (NULL != jObj) {
            methodInfo.env->DeleteLocalRef(jObj);
        }
        methodInfo.env->DeleteLocalRef(methodInfo.classID);
    }

    return responed;
}

char* HTTPRequest::closeJava() {
    JniMethodInfo methodInfo;
    if (JniHelper::getStaticMethodInfo(methodInfo,
        "org/cocos2dx/lib/QuickHTTPInterface",
        "close",
        "(Ljava/net/HttpURLConnection;)V"))
    {
        methodInfo.env->CallStaticVoidMethod(
            methodInfo.classID, methodInfo.methodID, m_httpConnect);
        methodInfo.env->DeleteLocalRef(methodInfo.classID);
    }
}

char* HTTPRequest::getCStrFromJString(jstring jstr, JNIEnv* env) {
    if (NULL == jstr) {
        return NULL;
    }

    const char* str = NULL;
    char* strRet = NULL;
    str = env->GetStringUTFChars(jstr, NULL);
    if (NULL != str) {
        strRet = strdup(str);
    }
    env->ReleaseStringUTFChars(jstr, str);

    return strRet;
}



NS_CC_EXTRA_END
