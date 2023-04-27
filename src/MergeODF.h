/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <OxOOL/Module/Base.h>

#include <Poco/Data/SQLite/Connector.h>
#include <Poco/Data/SessionPool.h>
#include <Poco/Data/Session.h>
#include <Poco/Data/RecordSet.h>
#include <Poco/JSON/Object.h>
#include <Poco/Net/HTMLForm.h>

struct RepositoryStruct
{
    unsigned long id = 0; // AUTOINCREMENT ID
    std::string cname = ""; // 範本資料夾名稱
    std::string endpt = ""; // 檔案代碼(存在 server 的檔名爲 endpt + "." + extname)
    std::string docname = ""; // 實際的檔名
    std::string extname = ""; // 副檔名
    std::string uptime = ""; // 上傳時間(比較像是檔案最後修改時間)
    unsigned long accessTimes = 0; // 呼叫次數
};

struct API
{
    // request method
    std::string method;
    // callback method
    std::function<void(const Poco::Net::HTTPRequest& request,
                       const std::shared_ptr<StreamSocket>& socket)> function;
};

struct DOCAPI
{
    // request method
    std::string method;
    // callback method
    std::function<void(const Poco::Net::HTTPRequest& request,
                        const std::shared_ptr<StreamSocket>& socket,
                        const RepositoryStruct& repo)> function;
};

// 更新資料庫行為
enum ActionType
{
    ADD = 0,
    UPDATE,
    DELETE
};

class MergeODF : public OxOOL::Module::Base
{
public:
    friend class Parser;

    MergeODF();

    ~MergeODF();

    void initialize() override;

    void handleRequest(const Poco::Net::HTTPRequest& request,
                       const std::shared_ptr<StreamSocket>& socket) override;

    std::string handleAdminMessage(const StringVector& tokens) override;

private:

    /// @brief 製作ODF報表檔
    /// @param request
    /// @param socket
    /// @param repo
    /// @param templateFile
    void makeODFReportFile(const Poco::Net::HTTPRequest& request,
                           const std::shared_ptr<StreamSocket>& socket,
                           const RepositoryStruct& repo,
                           const std::string& templateFile);

    /// @brief 寫入轉檔紀錄
    /// @param socket
    /// @param state true:成功, false: 失敗
    /// @param endpt endpoint
    /// @param message 訊息
    /// @param toPDF 是否輸出成 PDF
    void log(const std::shared_ptr<StreamSocket>& socket,
             const bool success,
             const RepositoryStruct& repo,
             const bool toPDF);

    /// @brief 保留字轉小寫
    std::string keyword2Lower(const std::string& in, const std::string& keyword);

    /// @brief 把 FORM 欄位，轉成 JSON 物件
    Poco::JSON::Object::Ptr parseArray2Form(const Poco::Net::HTMLForm& form);

    /// @brief 更新範本呼叫次數(+1)
    void updateAccessTimes(const std::string& endpt);

    void apiHelper(const Poco::Net::HTTPRequest& request,
                   const std::shared_ptr<StreamSocket>& socket, const bool showMerge = true,
                   const std::string& mergeEndPoint = std::string(), const bool anotherJson = false,
                   const bool yaml = false);

    std::string makeApiJson(const std::string& host,
                            const std::string& which = std::string(),
                            const bool anotherJson = false, const bool yaml = false,
                            const bool showHead = true);

    std::list<std::string> templLists(const bool isBasename);

private:

    /// @brief 取得可用的 data session
    /// @return Poco::Data::Session
    Poco::Data::Session getDataSession();

    /// @brief 取得符合 endpt 的紀錄
    /// @param endpt
    /// @return RepositoryStruct
    RepositoryStruct getRepository(std::string& endpt);

    /// @brief 更新範本資料表
    /// @param RepositoryStruc
    /// @return
    bool updateRepositoryData(ActionType type, RepositoryStruct& repo);

    /// @brief 取得範本倉庫路徑
    /// @return
    const std::string& getRepositoryPath();

private:

    std::map<std::string, API> mApiMap;
    /// @brief 初始化固定 URI
    void initApiMap();

    void okAPI(const Poco::Net::HTTPRequest& request,
               const std::shared_ptr<StreamSocket>& socket);

    void apiListsAPI(const Poco::Net::HTTPRequest& request,
                     const std::shared_ptr<StreamSocket>& socket);

    void yamlListsAPI(const Poco::Net::HTTPRequest& request,
                      const std::shared_ptr<StreamSocket>& socket);

    void listAPI(const Poco::Net::HTTPRequest& request,
                 const std::shared_ptr<StreamSocket>& socket);

    void uploadAPI(const Poco::Net::HTTPRequest& request,
                   const std::shared_ptr<StreamSocket>& socket);

    void updateAPI(const Poco::Net::HTTPRequest& request,
                   const std::shared_ptr<StreamSocket>& socket);

    void deleteAPI(const Poco::Net::HTTPRequest& request,
                   const std::shared_ptr<StreamSocket>& socket);

    void downloadAPI(const Poco::Net::HTTPRequest& request,
                     const std::shared_ptr<StreamSocket>& socket);


    std::map<std::string, DOCAPI> mDocApiMap;
    void initDocApiMap();

    void docApi(const Poco::Net::HTTPRequest& request,
                const std::shared_ptr<StreamSocket>& socket,
                const RepositoryStruct& repo);

    void docYaml(const Poco::Net::HTTPRequest& request,
                 const std::shared_ptr<StreamSocket>& socket,
                 const RepositoryStruct& repo);

    void docJson(const Poco::Net::HTTPRequest& request,
                 const std::shared_ptr<StreamSocket>& socket,
                 const RepositoryStruct& repo);

    void docAccessTimes(const Poco::Net::HTTPRequest& request,
                        const std::shared_ptr<StreamSocket>& socket,
                        const RepositoryStruct& repo);

private:

    static std::string TEMPLH;

    static std::string APITEMPL;

    static std::string YAMLTEMPLH;

    static std::string YAMLTEMPL;
};

std::string MergeODF::TEMPLH = R"(
{
    "swagger": "2.0",
    "info": {
        "version": "v1",
        "title": "ODF report API",
        "description": "Apply the data in JSON format to the template and output it as an Open Document Format file."
    },
    "host": "%s",
    "paths": {
        %s
    },
    "schemes": [
        "http",
        "https"
    ],
    "parameters": {
        "outputPDF": {
            "in": "query",
            "name": "outputPDF",
            "required": false,
            "type": "boolean",
            "allowEmptyValue": true,
            "description": "Output to PDF format."
        }
    }
}
    )";

std::string MergeODF::APITEMPL = R"(
        "/lool/mergeodf/%s/accessTimes": {
            "get": {
                "consumes": [
                    "multipart/form-data",
                    "application/json"
                ],
                "responses": {
                    "200": {
                    "description": "Success",
                    "schema": {
                        "type": "object",
                        "properties": {
                        "call_times": {
                            "type": "integer",
                            "description": "Number of calls."
                        }
                        }
                    }
                    },
                    "503": {
                        "description": "server_name 無指定"
                    },
                }
            }
        },
        "/lool/mergeodf/%s": {
            "post": {
                "consumes": [
                    "multipart/form-data",
                    "application/json"
                ],
                "parameters": [
                    {
                    "$ref": "#/parameters/outputPDF"
                    },
                    {
                    "in": "body",
                    "name": "body",
                    "description": "",
                    "required": true,
                    "schema": {
                        "type": "object",
                        "properties": {
                            %s
                        }
                    }
                ],
                "responses": {
                    "200": {
                        "description": "傳送成功"
                    },
                    "400": {
                        "description": "Json 格式錯誤 / form 格式錯誤"
                    },
                    "404": {
                        "description": "無此 API"
                    },
                    "500": {
                        "description": "轉換失敗 / 輸出 PDF 錯誤"
                    },
                    "503": {
                        "description": "模組尚未授權"
                    }

                }
            }
        }
    )";

std::string MergeODF::YAMLTEMPLH = R"(
swagger: '2.0'
info:
    version: v1
    title: ODF 報表 API
    description: ''
host: %s
paths:%s
schemes: ["http", "https"]
parameters:
    outputPDF:
    in: query
    name: outputPDF
    required: false
    type: boolean
    allowEmptyValue : true
    description: 轉輸出成 PDF 格式
    )";

std::string MergeODF::YAMLTEMPL = R"(
    /lool/mergeodf/%s/accessTimes:
        get:
            consumes:
            - application/json
            responses:
            '200':
                description: 傳送成功
                schema:
                    type: object
                    properties:
                        call_times:
                            type: integer
                            description: 呼叫次數
            '503':
                description: "server_name 無指定"
    /lool/mergeodf/%s:
        post:
            consumes:
                - multipart/form-data
                - application/json
            parameters:
                - $ref: '#/parameters/outputPDF'
                - in: body
                    name: body
                    description: ''
                    required: false
                    schema:
                        type: object
                        properties:
                            %s
            responses:
                '200':
                    description: '傳送成功'
                '400':
                    description: 'Json 格式錯誤 / form 格式錯誤'
                '404':
                    description: '無此 API'
                '500':
                    description: '轉換失敗 / 輸出 PDF 錯誤'
                '503':
                    description: '模組尚未授權'
    )";

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
