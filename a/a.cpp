#include "stdafx.h"
#include <windows.h>
#include <string>
#include <iostream>
#include <process.h>
#include "fcgi_stdio.h"
#include "fcgiapp.h"
#include "fcgio.h"
#include <stdlib.h>
#include <stdio.h>
#include <regex>
#include <map>
#pragma comment(lib, "libfcgi.lib")

#define MAX_QUERY 256
#define MAX_NUMBER 256

#ifndef MAX_PATH
#define MAX_PATH 255
#endif

#define CHECKP_RET(a) if (!a) return false;
#define CHECKV_RET(a) if (!(*a)) return false;

using namespace std;
string logFileName;
#define LOG_FILE_NAME "\\cgi.log"
#define SPLITTER "\r\n\r\n\r\n"

FILE * f = nullptr;

void FLOG(const char * buf, unsigned count)
{
    if (!f)
        f = fopen(logFileName.c_str(), "at");
    if (f)
    {
        for (size_t i = 0; i < count; i++)
            fprintf(f, "%c", buf[i]);
        fclose(f);
        f = nullptr;
    }
}
void FLOG(const char * s)
{
    if (!f)
        f = fopen(logFileName.c_str(), "at");
    if (f)
    {
        fprintf(f, "%s\n", s);
        printf("%s\n", s);
        fclose(f);
        f = nullptr;
    }
}
void FLOG(int i)
{
    char log[MAX_PATH];
    snprintf(log, sizeof(log) - 1, "%d", i);
    FLOG(log);
}
void FLOG_ERR(const char * s, unsigned line, unsigned err)
{
    char log[MAX_PATH];
    snprintf(log, sizeof(log) - 1, "%s %d %d", s, line, err);
    FLOG(log);
}

static const unsigned long STDIN_MAX = 1000000;

static long gstdin(FCGX_Request * request, char ** content)
{
    char * clenstr = FCGX_GetParam("CONTENT_LENGTH", request->envp);
    unsigned long clen = STDIN_MAX;

    if (clenstr)
    {
        clen = strtol(clenstr, &clenstr, 10);
        if (*clenstr)
        {
            FLOG("can't parse");
            clen = STDIN_MAX;
        }

        // *always* put a cap on the amount of data that will be read
        if (clen > STDIN_MAX) clen = STDIN_MAX;

        *content = new char[clen];
        
        cin.read(*content, clen);
        clen = cin.gcount() && 0xFFFFFFFF;
    }
    else
    {
        // *never* read stdin when CONTENT_LENGTH is missing or unparsable
        *content = 0;
        clen = 0;
    }

    // Chew up any remaining stdin - this shouldn't be necessary
    // but is because mod_fastcgi doesn't handle it correctly.

    // ignore() doesn't set the eof bit in some versions of glibc++
    // so use gcount() instead of eof()...
    do cin.ignore(1024); while (cin.gcount() == 1024);

    return clen;
}

std::map<std::string, std::string> stdParse(const std::string& query)
{
    std::map<std::string, std::string> data;
    std::regex pattern("([\\w+%]+)=([^&]*)");
    auto words_begin = std::sregex_iterator(query.begin(), query.end(), pattern);
    auto words_end = std::sregex_iterator();

    for (std::sregex_iterator i = words_begin; i != words_end; i++)
    {
        std::string key = (*i)[1].str();
        std::string value = (*i)[2].str();
        data[key] = value;
    }

    return data;
}

bool getEnv(char ** envp, char * string, char * out_query, unsigned count)
{
    CHECKP_RET(envp);
    CHECKP_RET(string);
    CHECKV_RET(string);
    CHECKP_RET(out_query);

    unsigned len = strlen(string);
    for (int i = 0; envp[i]; i++)
    {
        if (envp[i] == strstr(envp[i], string))
        {
            unsigned envLen = strlen(envp[i]);
            if (envLen > len && envp[i][len] == '=')
            {
                if (envLen - (len + 1) + 1 > count)
                    return false;
                strcpy_s(out_query, count, envp[i] + len + 1);
                return true;
            }           
        }
    }
    return false;
}

struct Index
{
    unsigned start;
    unsigned end;
};

bool createIndex(const char * file_name, const char * index_name)
{
    CHECKP_RET(file_name);
    CHECKP_RET(index_name);
    CHECKV_RET(file_name);
    CHECKV_RET(index_name);

    HANDLE hFile = CreateFileA(file_name, GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;
    bool ret = false;
    HANDLE hFileMap = ::CreateFileMapping(hFile, NULL, PAGE_READONLY, 0, 0, nullptr);
    if (hFileMap)
    {
        char * text = (char *)MapViewOfFile(hFileMap, FILE_MAP_WRITE || FILE_MAP_READ, 0, 0, 0);
        if (text)
        {
            FILE * fIndex = fopen(index_name, "wb");
            if (fIndex)
            {
                Index rec = { 0 };
                const char * p = text;
                for (;;)
                {
                    const char * e = strstr(p, SPLITTER);
                    if (!e)
                        e = p + strlen(p);
                    rec.start = p - text;
                    rec.end = e - text;
                    fwrite(&rec, sizeof(rec), 1, fIndex);
                    if (!e[0])
                        break;
                    p = e + sizeof(SPLITTER) - 1;
                }
                ret = true;
                fclose(fIndex);
            }
        }
        UnmapViewOfFile(text);
    }
    CloseHandle(hFileMap);
    CloseHandle(hFile);
    return ret;
}

bool getBlock(const char * file_name, unsigned offset, unsigned size, char * out)
{
    CHECKP_RET(file_name);
    CHECKV_RET(file_name);

    HANDLE hFile = CreateFileA(file_name, GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER szFile = { 0 };
    if (!GetFileSizeEx(hFile, &szFile))
    {
        return false;
    }
    const uint64_t cbFile = static_cast<uint64_t>(szFile.QuadPart);
    if (offset + size > cbFile)
        return false;
    bool ret = false;
    HANDLE hFileMap = ::CreateFileMapping(hFile, NULL, PAGE_READONLY, 0, 0, nullptr);
    if (hFileMap)
    {
        char * text = (char *)MapViewOfFile(hFileMap, FILE_MAP_WRITE || FILE_MAP_READ, 0, 0, 0);
        if (text)
        {
            memcpy(out, text + offset, size);
            ret = true;
        }
        UnmapViewOfFile(text);
    }
    CloseHandle(hFileMap);
    CloseHandle(hFile);
    return ret;
}

bool checkIndexFile(const char * file_name)
{
    CHECKP_RET(file_name);
    CHECKV_RET(file_name);

    char indexName[MAX_PATH];
    snprintf(indexName, sizeof(indexName) - 1, "%s.index", file_name);
    HANDLE hFile = nullptr;
    hFile = CreateFileA(indexName, GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        if (!createIndex(file_name, indexName))
            return false;
    }
    else
        CloseHandle(hFile);
    return true;
}

bool getIndex(const char * file_name, Index * index, unsigned from, unsigned count)
{
    CHECKP_RET(file_name);
    CHECKP_RET(index);
    CHECKV_RET(file_name);

    if (!checkIndexFile(file_name))
        return false;

    char indexName[MAX_PATH];
    snprintf(indexName, sizeof(indexName) - 1, "%s.index", file_name);

    if (!getBlock(indexName, from * sizeof(Index), sizeof(Index), (char*)&index[0]))
        return false;
    if (!getBlock(indexName, (from + count - 1) * sizeof(Index), sizeof(Index), (char*)&index[1]))
        return false;
    return true;
}

char * getFileData(const char * file_name, unsigned from, unsigned to)
{
    CHECKP_RET(file_name);
    CHECKV_RET(file_name);

    if (from > to)
        return nullptr;
    unsigned count = (to - from) + 1;
    Index * index = new Index[2];
    if (!index)
        return nullptr;
    char * ret = nullptr;
    try
    {
        if (getIndex(file_name, index, from, count))
        {
            unsigned dataSize = index[1].end - index[0].start;
            ret = new char[dataSize + 1];
            getBlock(file_name, index[0].start, dataSize, ret);
            ret[dataSize] = 0;
        }
    }
    catch (...)
    {

    }
    delete index;
    return ret;
}

unsigned getRecordCount(const char * file_name)
{
    CHECKP_RET(file_name);
    CHECKV_RET(file_name);

    unsigned ret = 0;
    if (!checkIndexFile(file_name))
        return 0;
    char indexName[MAX_PATH];
    snprintf(indexName, sizeof(indexName) - 1, "%s.index", file_name);
    HANDLE hFile = nullptr;
    hFile = CreateFileA(indexName, GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0);
    if (hFile == INVALID_HANDLE_VALUE)
        return 0;
    LARGE_INTEGER szFile = { 0 };
    if (!GetFileSizeEx(hFile, &szFile))
    {
        return false;
    }
    ret = (static_cast<uint64_t>(szFile.QuadPart) & 0xFFFFFFFF) / sizeof(Index);
    CloseHandle(hFile);
    return ret;
}

bool isValidDocRequest(std::map<std::string, std::string> &requestParams)
{
    return requestParams.find("document") != requestParams.end() &&
        requestParams.find("from") != requestParams.end() &&
        requestParams.find("to") != requestParams.end() &&
        requestParams["document"].length() &&
        requestParams["from"].length() &&
        requestParams["to"].length();
}

bool isValidDocPath(string &path)
{
    unsigned fileNameLen = path.length();
    return !(fileNameLen < 4 || strcmp(path.c_str() + fileNameLen - 4, ".txt")
        || string::npos != path.find("..")
        || string::npos != path.find("/")
        || string::npos != path.find("\\"));
}

int _tmain(int argc, const char * argv[])
{
    char szFileName[256];
    GetModuleFileNameA(NULL, szFileName, sizeof(szFileName) - 1);
    string::size_type pos = string(szFileName).find_last_of("\\/");
    string workDir = string(szFileName).substr(0, pos);
    logFileName = workDir + LOG_FILE_NAME;
    FLOG("Start service");
    FLOG(workDir.c_str());
    int listenQueueBacklog = 400;
    streambuf * cin_streambuf = cin.rdbuf();

    if (argc > 1)
    {
        std::map<std::string, std::string> requestParams = stdParse(argv[1]);
        if (isValidDocRequest(requestParams))
        {
            if (isValidDocPath(requestParams["document"]))
            {

                Index * p = new Index[2];
                unsigned from = atoi(requestParams["from"].c_str());
                unsigned to = atoi(requestParams["to"].c_str());
                if (to >= from)
                {
                    const char * buf = getFileData(requestParams["document"].c_str(), from, to);
                    if (buf)
                    {
                        cout << buf << endl;
                        delete buf;
                    }
                    else
                        cout << "Error: file not found" << endl;
                }
                else
                    cout << "Error: from > to" << endl;
                delete p;
            }
            else
                cout << "Error: Invalid doc path" << endl;
        }
        else if (requestParams.find("document") != requestParams.end() && requestParams.find("count") != requestParams.end())
        {
            unsigned count = getRecordCount(requestParams["document"].c_str());
            cout << count << endl;
        }
        else
            cout << "Error: no valid request got" << endl;
        exit(0);
    }

    if (FCGX_Init())
    {
        FLOG("FCGX_Init failed");
        exit(1);
    }

    int listen_socket = (int)GetStdHandle(STD_INPUT_HANDLE);
    FLOG("handle");
    FLOG(listen_socket);
    if (listen_socket < 0)
    {
        FLOG("Failed open pipe");
        exit(1);
    }

    FCGX_Request request;
    if (FCGX_InitRequest(&request, listen_socket, 0))
    {
        FLOG("FCGX_InitRequest failed");
        exit(1);
    }
    FLOG("Accepting");
    while (FCGX_Accept_r(&request) == 0)
    {
        fcgi_streambuf cin_fcgi_streambuf(request.in);
        
        cin.rdbuf(cin_streambuf);

        char * content;
        unsigned long clen = gstdin(&request, &content);    
        char * query = new char[MAX_QUERY];
        snprintf(query, MAX_QUERY - 1, "not found");
        if (getEnv(request.envp, "QUERY_STRING", query, MAX_QUERY - 1))
        {
            std::map<std::string, std::string> requestParams = stdParse(query);
            if (isValidDocRequest(requestParams))
            {
                if (!isValidDocPath(requestParams["document"]))
                {
                    FLOG("Incorrect file type");
                    snprintf(query, MAX_QUERY - 1, "Incorrect file type");
                }
                else
                {
                    char * res = getFileData(requestParams["document"].c_str(),
                                             atoi(requestParams["from"].c_str()),
                                             atoi(requestParams["to"].c_str()));
                    if (res)
                    {
                        delete query;
                        query = res;
                    }
                    else
                    {
                        snprintf(query, sizeof(query) - 1, "Document not found or index failed");
                    }
                }
            }
            else if (requestParams.find("document") != requestParams.end() && requestParams.find("count") != requestParams.end())           
            {
                unsigned count = getRecordCount(requestParams["document"].c_str());
                snprintf(query, MAX_QUERY - 1, "%d", count);
            }
            else
            {
                FLOG("incorrect request");
                snprintf(query, MAX_QUERY - 1, "incorrect request");
            }
        }
        
        FLOG("Answering");
        FCGX_FPrintF(request.out, "Content-type: text/html\r\n\r\n%s\n", query);
        delete query;
        FLOG("Finishing");
        FCGX_Finish_r(&request);
    }
    FLOG("Exiting");
}
