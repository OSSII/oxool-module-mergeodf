/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <OxOOL/ModuleManager.h>
#include <OxOOL/Module/Base.h>
#include <OxOOL/HttpHelper.h>
#include <OxOOL/ConvertBroker.h>

#include "MergeODF.h"
#include "MergeODFParser.h"

#include <Poco/RegularExpression.h>
#include <Poco/Glob.h>
#include <Poco/StringTokenizer.h>
#include <Poco/MemoryStream.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/HTMLForm.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>

using namespace Poco::Data::Keywords;


MergeODF::MergeODF()
{
    // 註冊 SQLite 連結
    Poco::Data::SQLite::Connector::registerConnector();
    // 初始化 API map
    initApiMap();
    // 初始化文件 API map
    initDocApiMap();
}

MergeODF::~MergeODF() { Poco::Data::SQLite::Connector::unregisterConnector(); }

void MergeODF::initialize()
{
    // 範本存放路徑
    const std::string repositoryPath = getRepositoryPath();
    // 路徑不存在就建立
    if (!Poco::File(repositoryPath).exists())
        Poco::File(repositoryPath).createDirectories();

    auto session = getDataSession();
    // 報表產生紀錄表
    session << "CREATE TABLE IF NOT EXISTS logging ("
            << "id        INTEGER PRIMARY KEY AUTOINCREMENT,"
            << "status    INTEGER NOT NULL DEFAULT 0,"
            << "to_pdf    INTEGER NOT NULL DEFAULT 0,"
            << "source_ip TEXT NOT NULL DEFAULT '',"
            << "file_name TEXT NOT NULL DEFAULT '',"
            << "file_ext  TEXT NOT NULL DEFAULT '',"
            << "timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP)",
        now;

    // ODF 報表範本對照檔
    session << "CREATE TABLE IF NOT EXISTS repository ("
            << "id      INTEGER PRIMARY KEY AUTOINCREMENT,"
            << "cname   TEXT NOT NULL DEFAULT '',"
            << "endpt   TEXT NOT NULL DEFAULT '' UNIQUE," // end point 名稱
            << "docname TEXT NOT NULL DEFAULT ''," // 主檔名
            << "extname TEXT NOT NULL DEFAULT ''," // 副檔名
            << "uptime  TEXT NOT NULL DEFAULT ''," // 上傳日期
            << "accessTimes INTEGER NOT NULL DEFAULT 0)", // 呼叫次數
        now;

    // 刪除超過一年的舊紀錄
    session << "DELETE FROM logging WHERE (strftime('%s', 'now') "
            << "- strftime('%s', timestamp)) > 86400 * 365",
        now;
}

void MergeODF::handleRequest(const Poco::Net::HTTPRequest& request,
                                const std::shared_ptr<StreamSocket>& socket)
{
    const std::string requestMethod = request.getMethod();
    const std::string requestAPI = parseRealURI(request);

    // 是否支援此 API(固定位址)
    if (auto it = mApiMap.find(requestAPI); it != mApiMap.end())
    {
        auto api = it->second;
        // 檢查 request 方法是否正確?
        if (requestMethod != api.method)
        {
            OxOOL::HttpHelper::sendErrorAndShutdown(
                Poco::Net::HTTPResponse::HTTP_METHOD_NOT_ALLOWED, socket);
            return;
        }

        api.function(request, socket); // 執行對應的 API
    }
    else // 沒有相對應的 API
    {
        // 切割 URL 字串
        const Poco::StringTokenizer tokens(requestAPI, "/",
                                            Poco::StringTokenizer::TOK_IGNORE_EMPTY
                                                | Poco::StringTokenizer::TOK_TRIM);
        const std::size_t tokenSize = tokens.count();
        //
        if (tokenSize > 0 && tokenSize <= 2)
        {
            // 檢查是否確實有該筆記錄及檔案存在
            std::string docId = tokens[0];
            RepositoryStruct repo = getRepository(docId);
            // 範本檔完整路徑
            const std::string templateFile = getRepositoryPath() + "/" + repo.endpt + "." + repo.extname;
            if (repo.id != 0 && Poco::File(templateFile).exists())
            {
                // 產製報表檔案
                if (tokenSize == 1)
                {
                    // 只接受 OPTIONS 或 POST
                    if (!OxOOL::HttpHelper::isOPTIONS(request) && !OxOOL::HttpHelper::isPOST(request))
                    {
                        OxOOL::HttpHelper::sendErrorAndShutdown(
                                Poco::Net::HTTPResponse::HTTP_METHOD_NOT_ALLOWED, socket);
                        return;
                    }

                    makeODFReportFile(request, socket, repo, templateFile);
                    return;
                }
                else // 列出與該報表有關的 api 資訊
                {
                    const std::string apiName = tokens[1]; // 取得 doc api 名稱
                    if (auto docIt = mDocApiMap.find(apiName); docIt != mDocApiMap.end())
                    {
                        auto docApi = docIt->second;
                        // 檢查 request 方法是否正確?
                        if (requestMethod != docApi.method)
                        {
                            OxOOL::HttpHelper::sendErrorAndShutdown(
                                Poco::Net::HTTPResponse::HTTP_METHOD_NOT_ALLOWED, socket);
                            return;
                        }

                        docApi.function(request, socket, repo); // 執行對應的 DOC API
                        return;
                    }
                }
            }
        }

        OxOOL::HttpHelper::sendErrorAndShutdown(Poco::Net::HTTPResponse::HTTP_NOT_FOUND, socket);
    }
}

std::string MergeODF::handleAdminMessage(const StringVector& tokens)
{
    // 傳回最新的紀錄
    if (tokens.equals(0, "refreshLog"))
    {
        auto session = getDataSession();

        // SQL query.
        Poco::Data::Statement select(session);
        select << "SELECT * FROM logging", now;
        Poco::Data::RecordSet rs(select);

        std::size_t cols = rs.columnCount(); // 取欄位數

        // 遍歷所有資料列
        std::string result("logData [");
        for (auto row : rs)
        {
            // 轉為 JSON 物件
            Poco::JSON::Object json;
            for (std::size_t col = 0; col < cols ; col++)
                json.set(rs.columnName(col), row.get(col));

            std::ostringstream oss;
            json.stringify(oss);
            // 轉為 json 字串
            result.append(oss.str()).append(",");
        }
        // 去掉最後一的 ',' 號
        if (rs.rowCount() > 0)
            result.pop_back();

        result.append("]");

        return result;

    }
    return "";
}

void MergeODF::makeODFReportFile(const Poco::Net::HTTPRequest& request,
                                 const std::shared_ptr<StreamSocket>& socket,
                                 const RepositoryStruct& repo,
                                 const std::string& templateFile)
{
    OxOOL::HttpHelper::KeyValueMap extraHeader;
    extraHeader["Access-Control-Allow-Origin"] = "*";
    extraHeader["Access-Control-Allow-Methods"] = "POST, OPTIONS";
    extraHeader["Access-Control-Allow-Headers"] = "Origin, X-Requested-With, Content-Type, Accept";

    // Swagger's CORS would send OPTIONS first to check if the server allow CROS,
    // So Check First OPTIONS and allow
    if (OxOOL::HttpHelper::isOPTIONS(request))
    {
        OxOOL::HttpHelper::sendResponseAndShutdown(socket, "",
            Poco::Net::HTTPResponse::HTTP_OK, "", extraHeader);
        return;
    }

    updateAccessTimes(repo.endpt); // 呼叫次數 +1

    // 是否要輸出 PDF
    Poco::Net::HTMLForm urlParam(request); // 網址列參數
    // 有帶 ?outputPDF 且不等於 false，表示要輸出爲 PDF 格式
    bool toPDF = (urlParam.has("outputPDF") && urlParam.get("outputPDF") != "false");

    Poco::JSON::Object::Ptr object;
    std::string jsonParseMessage;
    Poco::MemoryInputStream message(&socket->getInBuffer()[0], socket->getInBuffer().size());
    // 讀取 POST 資料
    // 直接傳遞 json 內容
    if (request.getContentType() == "application/json")
    {
        std::string line, data;
        std::istream &iss(message);
        while (!iss.eof())
        {
            std::getline(iss, line);
            data += line;
        }
        // 解析 request body to json
        std::string jstr = data;

        jstr = keyword2Lower(jstr, "null");
        jstr = keyword2Lower(jstr, "true");
        jstr = keyword2Lower(jstr, "false");

        Poco::JSON::Parser jparser;
        jparser.setAllowComments(true);
        Poco::Dynamic::Var result;

        // Parse data to PocoJSON
        try
        {
            result = jparser.parse(jstr);
            object = result.extract<Poco::JSON::Object::Ptr>();
        }
        catch (Poco::Exception& e)
        {
            jsonParseMessage = "Json format error";
        }
    }
    else
    {
        Poco::Net::HTMLForm form;
        OxOOL::HttpHelper::PartHandler partHandler;
        // 不限制欄位數量
        form.setFieldLimit(0);
        // 讀取 HTTML Form.
        form.load(request, message, partHandler);

        // 資料形式如果是 post HTML Form 上來
        try
        {
            object = parseArray2Form(form);
        }
        catch (Poco::Exception& e)
        {
            jsonParseMessage = "Form format error.";
        }
    }

    // 解析錯誤就傳回 HTTP code 400
    if (!jsonParseMessage.empty())
    {
        OxOOL::HttpHelper::sendErrorAndShutdown(
            Poco::Net::HTTPResponse::HTTPStatus::HTTP_BAD_REQUEST, socket, jsonParseMessage);
        log(socket,false, repo, toPDF);
        return;
    }

    std::shared_ptr<Parser> parser = std::make_shared<Parser>();
    parser->extract(templateFile); // 解壓縮範本檔

    // XML 前處理:  遍歷文件的步驟都要在這裡處理,不然隨著文件的內容增加,會導致遍歷時間大量增長
    //把 form 的資料放進 xml 檔案
    auto allVar = parser->scanVarPtr();
    std::list<Poco::XML::Element*> singleVar = allVar[0];
    std::list<Poco::XML::Element*> groupVar = allVar[1];

    parser->setSingleVar(object, singleVar);
    parser->setGroupVar(object, groupVar);
    const auto zip2 = parser->zipback();

    if (!toPDF)
    {
        const std::string mimeType = OxOOL::HttpHelper::getMimeType(zip2);
        Poco::Net::HTTPResponse response;
        for (auto it : extraHeader)
        {
            response.set(it.first, it.second);
        }
        response.set("Content-Disposition", "attachment; filename=\"" + zip2 + "\"");
        OxOOL::HttpHelper::sendFileAndShutdown(socket, zip2, mimeType, &response, true);
        Poco::File(zip2).remove(); // 移除檔案
    }
    else
    {
        LOG_INF(logTitle() << "Convert " << zip2 << " to PDF.");
        // 取得轉檔用的 Broker
        auto docBroker = OxOOL::ConvertBroker::create(zip2, "pdf");
        // 以唯讀開啟
        if (!docBroker->loadDocumentReadonly(socket))
        {
            LOG_ERR(logTitle() << "Failed to create Client Session on docKey ["
                               << docBroker->getDocKey() << "].");
            log(socket, false, repo, toPDF);
            return;
        }
    }

    log(socket, true, repo, toPDF);
}

void MergeODF::log(const std::shared_ptr<StreamSocket>& socket,
                   const bool success,
                   const RepositoryStruct& repo,
                   const bool toPDF)
{
    bool status = success;
    bool to_pdf = toPDF;
    std::string file_name = repo.docname;
    std::string file_ext = repo.extname;

    // 來源 IP
    std::string sourceIP = socket->clientAddress();
    auto session = getDataSession();
    session << "INSERT INTO logging (status, to_pdf, source_ip, file_name, file_ext) "
            << "VALUES(?, ?, ?, ?, ?)",
            use(status), use(to_pdf), use(sourceIP), use(file_name), use(file_ext), now;
}

/// json: 關鍵字轉小寫，但以quote包起來的字串不處理
std::string MergeODF::keyword2Lower(const std::string& in, const std::string& keyword)
{
    std::string newStr = in;

    Poco::RegularExpression re(keyword, Poco::RegularExpression::RE_CASELESS);
    Poco::RegularExpression::Match match;

    auto matchSize = re.match(newStr, 0, match);

    while (matchSize > 0)
    {
        // @TODO: add check for "   null   "
        if (newStr[match.offset - 1] != '"' &&
                newStr[match.offset + keyword.size()] != '"')
        {
            for(unsigned idx = 0; idx < keyword.size(); idx ++)
                newStr[match.offset + idx] = keyword[idx];
        }
        matchSize = re.match(newStr, match.offset + match.length, match);
    }
    return newStr;
}

/// 解析表單陣列： 詳細資料[0][姓名] => 詳細資料:姓名
Poco::JSON::Object::Ptr MergeODF::parseArray2Form(const Poco::Net::HTMLForm& form)
{
    // {"詳細資料": [ {"姓名": ""} ]}
    std::map <std::string,
        std::vector<std::map<std::string, std::string>>
            > grpNames;
    // 詳細資料[0][姓名] => {"詳細資料": [ {"姓名": ""} ]}
    Poco::JSON::Object::Ptr formJson = new Poco::JSON::Object();

    for (auto iterator = form.begin();
            iterator != form.end();
            iterator ++)
    {
        const auto varname = iterator->first;
        const auto value = iterator->second;

        auto res = "^([^\\]\\[]*)\\[([^\\]\\[]*)\\]\\[([^\\]\\[]*)\\]$";
        Poco::RegularExpression re(res);
        Poco::RegularExpression::MatchVec posVec;
        re.match(varname, 0, posVec);
        if (posVec.empty())
        {
            formJson->set(varname, Poco::Dynamic::Var(value));
            continue;
        }

        const auto grpname = varname.substr(posVec[1].offset,
                posVec[1].length);
        const auto grpidxRaw = varname.substr(posVec[2].offset,
                posVec[2].length);
        const auto grpkey = varname.substr(posVec[3].offset,
                posVec[3].length);
        const int grpidx = std::stoi(grpidxRaw);

        //std::vector<std::map<std::string, std::string>> dummy;
        if (grpNames.find(grpname) == grpNames.end())
        {  // default array
            std::vector<std::map<std::string, std::string>> dummy;
            grpNames[grpname] = dummy;
        }

        // 詳細資料[n][姓名]: n to resize
        // n 有可能 1, 3, 2, 6 不照順序, 這裡以 n 當作 resize 依據
        // 就可以調整陣列大小了
        if (grpNames[grpname].size() < (unsigned)(grpidx + 1))
            grpNames[grpname].resize(grpidx + 1);
        grpNames[grpname].at(grpidx)[grpkey] = value;
    }
    // {"詳細資料": [ {"姓名": ""} ]} => 詳細資料:姓名=value
    //
    for(auto itgrp = grpNames.begin();
            itgrp != grpNames.end();
            itgrp++)
    {
        auto gNames = itgrp->second;
        for(unsigned grpidx = 0; grpidx < gNames.size(); grpidx ++)
        {
            auto names = gNames.at(grpidx);
            Poco::JSON::Object::Ptr tempData = new Poco::JSON::Object();
            for(auto itname = names.begin();
                    itname != names.end();
                    itname++)
            {
                tempData->set(itname->first, Poco::Dynamic::Var(itname->second));
            }
            if (names.size() != 0 )
            {
                if(!formJson->has(itgrp->first))
                {
                    Poco::JSON::Array::Ptr newArr = new Poco::JSON::Array();
                    formJson->set(itgrp->first, newArr);
                }
                formJson->getArray(itgrp->first)->add(tempData);
            }
        }
    }
    return formJson;
}

void MergeODF::updateAccessTimes(const std::string& endpt)
{
    auto session = getDataSession();

    std::string docId = endpt;
    session << "UPDATE repository set accessTimes = accessTimes + 1 WHERE endpt=?",
            use(docId), now;
}

void MergeODF::initApiMap()
{
    mApiMap = { { // 回應 HTTP OK
                    "/",
                    {
                        method : Poco::Net::HTTPRequest::HTTP_GET,
                        function : std::bind(&MergeODF::okAPI, this, std::placeholders::_1,
                                            std::placeholders::_2)
                    } },
                { // 取得所有 api 列表
                    "/api",
                    {
                        method : Poco::Net::HTTPRequest::HTTP_GET,
                        function : std::bind(&MergeODF::apiListsAPI, this, std::placeholders::_1,
                                            std::placeholders::_2)
                    } },
                { // 取得所有 yaml 列表
                    "/yaml",
                    {
                        method : Poco::Net::HTTPRequest::HTTP_GET,
                        function : std::bind(&MergeODF::yamlListsAPI, this, std::placeholders::_1,
                                            std::placeholders::_2)
                    } },
                { // 取得所有範本列表
                    "/list",
                    {
                        method : Poco::Net::HTTPRequest::HTTP_GET,
                        function : std::bind(&MergeODF::listAPI, this, std::placeholders::_1,
                                            std::placeholders::_2)
                    } },
                { // 收取範本檔
                    "/upload",
                    {
                        method : Poco::Net::HTTPRequest::HTTP_POST,
                        function : std::bind(&MergeODF::uploadAPI, this, std::placeholders::_1,
                                            std::placeholders::_2)
                    } },
                { // 更新範本檔
                    "/update",
                    {
                        method : Poco::Net::HTTPRequest::HTTP_POST,
                        function : std::bind(&MergeODF::updateAPI, this, std::placeholders::_1,
                                            std::placeholders::_2)
                    } },
                { // 刪除範本檔
                    "/delete",
                    {
                        method : Poco::Net::HTTPRequest::HTTP_POST,
                        function : std::bind(&MergeODF::deleteAPI, this, std::placeholders::_1,
                                            std::placeholders::_2)
                    } },
                { // 下載範本檔
                    "/download",
                    {
                        method : Poco::Net::HTTPRequest::HTTP_POST,
                        function : std::bind(&MergeODF::downloadAPI, this, std::placeholders::_1,
                                            std::placeholders::_2)
                    } } };
}

void MergeODF::initDocApiMap()
{
    mDocApiMap
        = { { // doc_id/api
                "api",
                {
                    method : Poco::Net::HTTPRequest::HTTP_GET,
                    function : std::bind(&MergeODF::docApi, this, std::placeholders::_1,
                                        std::placeholders::_2, std::placeholders::_3)
                } },
            { // doc_id/yaml
                "yaml",
                {
                    method : Poco::Net::HTTPRequest::HTTP_GET,
                    function : std::bind(&MergeODF::docYaml, this, std::placeholders::_1,
                                        std::placeholders::_2, std::placeholders::_3)
                } },
            { // doc_id/json
                "json",
                {
                    method : Poco::Net::HTTPRequest::HTTP_GET,
                    function : std::bind(&MergeODF::docJson, this, std::placeholders::_1,
                                        std::placeholders::_2, std::placeholders::_3)
                } },
            { // doc_id/accessTimes
                "accessTimes",
                {
                    method : Poco::Net::HTTPRequest::HTTP_GET,
                    function : std::bind(&MergeODF::docAccessTimes, this, std::placeholders::_1,
                                        std::placeholders::_2, std::placeholders::_3)
                } } };
}

void MergeODF::docApi(const Poco::Net::HTTPRequest& request,
            const std::shared_ptr<StreamSocket>& socket,
            const RepositoryStruct& repo)
{
    apiHelper(request, socket, true, repo.endpt);
}

void MergeODF::docYaml(const Poco::Net::HTTPRequest& request,
                       const std::shared_ptr<StreamSocket>& socket,
                       const RepositoryStruct& repo)
{
    apiHelper(request, socket, true, repo.endpt, false, true);
}

void MergeODF::docJson(const Poco::Net::HTTPRequest& request,
                       const std::shared_ptr<StreamSocket>& socket,
                       const RepositoryStruct& repo)
{
    apiHelper(request, socket, true, repo.endpt, true);
}

void MergeODF::docAccessTimes(const Poco::Net::HTTPRequest& /* request */,
                              const std::shared_ptr<StreamSocket>& socket,
                              const RepositoryStruct& repo)
{
    std::string jsonStr("{\"call_times\":" + std::to_string(repo.accessTimes) + "}");
    OxOOL::HttpHelper::sendResponseAndShutdown(
        socket, jsonStr, Poco::Net::HTTPResponse::HTTP_OK, "application/json");
}

void MergeODF::okAPI(const Poco::Net::HTTPRequest& /*request*/,
            const std::shared_ptr<StreamSocket>& socket)
{
    OxOOL::HttpHelper::sendResponseAndShutdown(socket);
}

void MergeODF::apiListsAPI(const Poco::Net::HTTPRequest& request,
                    const std::shared_ptr<StreamSocket>& socket)
{
    apiHelper(request, socket);
}

void MergeODF::yamlListsAPI(const Poco::Net::HTTPRequest& request,
                    const std::shared_ptr<StreamSocket>& socket)
{
    apiHelper(request, socket, true, "", false, true);
}

void MergeODF::listAPI(const Poco::Net::HTTPRequest& /*request*/,
                const std::shared_ptr<StreamSocket>& socket)
{
    auto session = getDataSession();

    // 查詢範本類別列表
    std::vector<std::string> groups;
    session << "SELECT cname FROM repository GROUP BY cname", into(groups), now;

    // 依據範本類別分組
    Poco::JSON::Object json;
    for (auto group : groups)
    {
        Poco::JSON::Array groupArray;
        // 查詢各組明細
        Poco::Data::Statement select(session);
        select << "SELECT docname, endpt, extname, uptime FROM repository WHERE cname=?",
            use(group), now;
        Poco::Data::RecordSet rs(select);

        std::size_t cols = rs.columnCount(); // 取欄位數
        // 遍歷所有資料列
        for (auto row : rs)
        {
            // 轉為 JSON 物件
            Poco::JSON::Object obj;
            for (std::size_t col = 0; col < cols; col++)
            {
                obj.set(rs.columnName(col), row.get(col));
            }
            // 加入該組陣列
            groupArray.add(obj);
        }

        // 完成一組
        json.set(group, groupArray);
    }

    std::ostringstream oss;
    json.stringify(oss, 4);
    OxOOL::HttpHelper::sendResponseAndShutdown(socket, oss.str());
}

void MergeODF::uploadAPI(const Poco::Net::HTTPRequest& request,
                const std::shared_ptr<StreamSocket>& socket)
{
    // 讀取 HTTML Form.
    OxOOL::HttpHelper::PartHandler partHandler;
    Poco::MemoryInputStream message(&socket->getInBuffer()[0], socket->getInBuffer().size());
    const Poco::Net::HTMLForm form(request, message, partHandler);

    // 從 form 取值
    RepositoryStruct repo = {
        cname : form.get("cname", ""),
        endpt : form.get("endpt", ""),
        docname : form.get("docname", ""),
        extname : form.get("extname", ""),
        uptime : form.get("uptime", "")
    };

    // 有收到檔案
    if (!partHandler.empty())
    {
        const Poco::File recivedFile(partHandler.getFilename());
        const std::string newName
            = getRepositoryPath() + "/" + form.get("endpt") + "." + form.get("extname");
        // 收到的檔案複製一份並改名，存到 RepositoryPath 路徑下
        recivedFile.copyTo(newName);
        // 移除收到的檔案
        partHandler.removeFiles();

        // 更新資料庫(新增)
        updateRepositoryData(ActionType::ADD, repo);

        OxOOL::HttpHelper::sendResponseAndShutdown(socket, "Upload Success.");
    }
    else // 沒有收到檔案
    {
        OxOOL::HttpHelper::sendErrorAndShutdown(Poco::Net::HTTPResponse::HTTP_BAD_REQUEST,
                                                socket, "File not received.");
    }
}

void MergeODF::updateAPI(const Poco::Net::HTTPRequest& request,
                const std::shared_ptr<StreamSocket>& socket)
{
    // 讀取 HTTML Form.
    OxOOL::HttpHelper::PartHandler partHandler;
    Poco::MemoryInputStream message(&socket->getInBuffer()[0], socket->getInBuffer().size());
    const Poco::Net::HTMLForm form(request, message, partHandler);

    // 有收到檔案
    if (!partHandler.empty())
    {
        // 收到的檔案
        const Poco::File recivedFile(partHandler.getFilename());
        // 讀取該筆原始記錄
        std::string endpt = form.get("endpt", "");
        RepositoryStruct repo = getRepository(endpt);
        // 確實有資料
        if (repo.id != 0)
        {
            Poco::File oldFile(getRepositoryPath() + "/" + repo.endpt + "." + repo.extname);
            // 舊檔案存在就刪除它
            if (oldFile.exists())
            {
                oldFile.remove();
            }
        }

        // 紀錄新資料
        repo.endpt = endpt;
        repo.extname = form.get("extname", "");
        repo.uptime = form.get("uptime", "");
        // 新的檔名應該要一樣
        const std::string newName = getRepositoryPath() + "/" + repo.endpt + "." + repo.extname;
        // 收到的檔案複製一份並改名，存到 RepositoryPath 路徑下
        recivedFile.copyTo(newName);

        // 更新資料庫(新增)
        updateRepositoryData(ActionType::UPDATE, repo);

        // 移除收到的檔案
        partHandler.removeFiles();

        OxOOL::HttpHelper::sendResponseAndShutdown(socket, "Update Success.");
    }
    else // 沒有收到檔案
    {
        OxOOL::HttpHelper::sendErrorAndShutdown(Poco::Net::HTTPResponse::HTTP_BAD_REQUEST,
                                                socket, "File not received.");
    }
}

void MergeODF::deleteAPI(const Poco::Net::HTTPRequest& request,
                const std::shared_ptr<StreamSocket>& socket)
{
    // 讀取 HTTML Form.
    Poco::MemoryInputStream message(&socket->getInBuffer()[0], socket->getInBuffer().size());
    const Poco::Net::HTMLForm form(request, message);

    // 從 form 取值
    RepositoryStruct repo
        = { endpt : form.get("endpt", ""), extname : form.get("extname", "") };

    if (repo.endpt.empty())
    {
        OxOOL::HttpHelper::sendErrorAndShutdown(Poco::Net::HTTPResponse::HTTP_BAD_REQUEST,
                                                socket, "No endpt provide.");
    }
    else
    {
        Poco::File targetFile(getRepositoryPath() + "/" + repo.endpt + "." + repo.extname);
        // 指定檔案存在
        if (targetFile.exists())
        {
            targetFile.remove(); // 刪除指定檔案
            // 更新資料庫(刪除)
            updateRepositoryData(ActionType::DELETE, repo);
            OxOOL::HttpHelper::sendResponseAndShutdown(socket, "Delete success.");
        }
        else
        {
            OxOOL::HttpHelper::sendErrorAndShutdown(Poco::Net::HTTPResponse::HTTP_NOT_FOUND,
                                                    socket,
                                                    "The file to be deleted does not exist");
        }
    }
}

void MergeODF::downloadAPI(const Poco::Net::HTTPRequest& request,
                    const std::shared_ptr<StreamSocket>& socket)
{
    // 讀取 HTTML Form.
    Poco::MemoryInputStream message(&socket->getInBuffer()[0], socket->getInBuffer().size());
    const Poco::Net::HTMLForm form(request, message);

    // 讀取範本紀錄
    std::string endpt = form.get("endpt", "");
    const RepositoryStruct repo = getRepository(endpt);

    // 沒有找到範本紀錄
    if (repo.id == 0)
    {
        OxOOL::HttpHelper::sendErrorAndShutdown(Poco::Net::HTTPResponse::HTTP_BAD_REQUEST,
                                                socket, "No endpt provide.");
    }
    else
    {
        Poco::File targetFile(getRepositoryPath() + "/" + repo.endpt + "." + repo.extname);
        // 指定檔案存在
        if (targetFile.exists())
        {
            Poco::Net::HTTPResponse response;
            response.set("Content-Disposition",
                            "attachment; filename=\"" + repo.docname + "." + repo.extname + '"');

            OxOOL::HttpHelper::sendFileAndShutdown(socket, targetFile.path(),
                                                    "application/octet-stream", &response);
        }
        else
        {
            OxOOL::HttpHelper::sendErrorAndShutdown(Poco::Net::HTTPResponse::HTTP_NOT_FOUND,
                                                    socket);
        }
    }
}

void MergeODF::apiHelper(const Poco::Net::HTTPRequest& request,
                const std::shared_ptr<StreamSocket>& socket, const bool showMerge,
                const std::string& mergeEndPoint, const bool anotherJson,
                const bool yaml)
{
    std::string paths = "";
    bool showHead = !(false && showMerge);
    if (showMerge)
    {
        auto merge = makeApiJson(request.getHost(), mergeEndPoint, anotherJson, yaml, showHead);
        if (!paths.empty() && !merge.empty() && !yaml)
            paths += ",";
        paths += merge;
    }

    const std::string read = paths;

    auto mediaType = !anotherJson && !yaml ? "application/json"
                   : !yaml ? "text/html; charset=utf-8"
                   : "text/plain; charset=utf-8";

    OxOOL::HttpHelper::sendResponseAndShutdown(socket, read, Poco::Net::HTTPResponse::HTTP_OK,
                                                mediaType);
}

std::string MergeODF::makeApiJson(const std::string& host,
                                  const std::string& which,
                                  const bool anotherJson, const bool yaml,
                                  const bool showHead)
{
    std::string jsonstr;

    auto templsts = templLists(false);
    auto it = templsts.begin();
    for (size_t pos = 0; it != templsts.end(); ++it, pos++)
    {
        try
        {
            const auto templateFile = *it;
            std::shared_ptr<Parser> parser = std::make_shared<Parser>();
            parser->extract(templateFile);
            parser->setOutputFlags(anotherJson, yaml);

            auto endpoint = Poco::Path(templateFile).getBaseName();

            if (!which.empty() && endpoint != which)
                continue;

            std::string buf;
            std::string parserResult;
            if (anotherJson)
            {
                buf = "* json 傳遞的 json 資料需以 urlencode(encodeURIComponent) 編碼<br />"
                        "* 圖檔需以 base64 編碼<br />"
                        "* 若以 json 傳參數，則 header 需指定 content-type='application/json'<br "
                        "/><br />json 範例:<br /><br />";
                buf += Poco::format("{<br />%s}", parser->jjsonVars());
            }
            else if (yaml)
                Poco::format(buf, YAMLTEMPL, endpoint, endpoint, parser->yamlVars());
            else
                Poco::format(buf, APITEMPL, endpoint, endpoint, parser->jsonVars());

            jsonstr += buf;

            if (!which.empty() && endpoint == which)
                break;

            if (pos != templsts.size() - 1 && !yaml)
                jsonstr += ",";
        }
        catch (const std::exception& e)
        {
        }
    }

    //cout << jsonstr << endl;
    // add header
    if (showHead && !anotherJson)
    {
        std::string read;
        Poco::format(read, (yaml ? YAMLTEMPLH : TEMPLH), host, jsonstr);
        return read;
    }

    return jsonstr;
}

std::list<std::string> MergeODF::templLists(const bool isBasename)
{
    std::set<std::string> files;
    std::list<std::string> rets;

    Poco::Glob::glob(getRepositoryPath() + "/*.ot[ts]", files);
    for (auto it : files)
    {
        if (isBasename)
            rets.push_back(Poco::Path(it).getBaseName());
        else
            rets.push_back(it);
    }
    return rets;
}

// private:
Poco::Data::Session MergeODF::getDataSession()
{
    static std::string dbName = getDocumentRoot() + "/data.db";
    static Poco::Data::SessionPool sessionPool("SQLite", dbName);
    return sessionPool.get();
}

RepositoryStruct MergeODF::getRepository(std::string& endpt)
{
    RepositoryStruct repo;

    auto session = getDataSession();
    try
    {
        session << "SELECT id, cname, docname, endpt, extname, uptime, accessTimes FROM repository WHERE "
                    "endpt=?",
            into(repo.id), into(repo.cname), into(repo.docname), into(repo.endpt),
            into(repo.extname), into(repo.uptime), into(repo.accessTimes), use(endpt), now;
    }
    catch (const Poco::Exception& exc)
    {

    }

    return repo;
}

bool MergeODF::updateRepositoryData(ActionType type, RepositoryStruct& repo)
{
    try
    {
        auto session = getDataSession();
        switch (type)
        {
            case ActionType::ADD: // 新增
                session << "INSERT INTO repository (endpt, extname, cname, docname, uptime) "
                        << "VALUES(?, ?, ?, ?, ?)",
                    use(repo.endpt), use(repo.extname), use(repo.cname), use(repo.docname),
                    use(repo.uptime), now;
                break;

            case ActionType::UPDATE: // 更新
                session << "UPDATE repository SET "
                        << "extname=?,"
                        << "uptime=?"
                        << " WHERE endpt=?",
                        use(repo.extname), use(repo.uptime), use(repo.endpt), now;
                break;

            case ActionType::DELETE: // 刪除
                session << "DELETE FROM repository WHERE endpt=?", use(repo.endpt), now;
                break;
        }
    }
    catch (const Poco::Exception& exc)
    {
        LOG_ERR("Admin module [" << getDetail().name
                                    << "] update database:" << exc.displayText());
        return false;
    }

    return true;
}

const std::string& MergeODF::getRepositoryPath()
{
    static std::string repositoryPath = getDocumentRoot() + "/repository";
    return repositoryPath;
}


OXOOL_MODULE_EXPORT(MergeODF);

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
