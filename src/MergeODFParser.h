/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <string>

#include <Poco/AutoPtr.h>
#include <Poco/URI.h>
#include <Poco/JSON/Object.h>
#include <Poco/StringTokenizer.h>
#include <Poco/DOM/Element.h>

#define TOKENOPTS (Poco::StringTokenizer::TOK_IGNORE_EMPTY | Poco::StringTokenizer::TOK_TRIM)

enum DocType
{
    OTHER,
    TEXT,
    SPREADSHEET
};

class Parser
{
public:
    Parser();
    ~Parser();

    std::string getMimeType();
    void extract(const std::string& templateFile);

    std::string jsonVars();
    std::string jjsonVars();
    std::string yamlVars();

    std::vector<std::list<Poco::XML::Element*>> scanVarPtr();
    std::string zipback();

    void updatePic2MetaXml();

    void setOutputFlags(bool, bool);
    std::string varKeyValue(std::string, std::string);
    void setSingleVar(Poco::JSON::Object::Ptr, std::list<Poco::XML::Element*>&);
    void setGroupVar(Poco::JSON::Object::Ptr, std::list<Poco::XML::Element*>&);
    std::string jsonvars; // json 說明 - openapi
    std::string jjsonvars; // json 範例
    std::string yamlvars; // yaml
private:
    DocType doctype;
    unsigned picserial;

    bool outAnotherJson;
    bool outYaml;

    std::map<std::string, Poco::Path> zipfilepaths;
    Poco::AutoPtr<Poco::XML::Document> docXML;
    std::list<Poco::XML::Element*> groupAnchorsSc;

    std::string extra2;
    std::string contentXml;
    std::string contentXmlFileName;
    std::string metaFileName;

    void detectDocType();
    bool isText();
    bool isSpreadSheet();

    std::string replaceMetaMimeType(std::string);
    void updateMetaInfo();

    std::string parseEnumValue(std::string, std::string, std::string);
    std::string parseJsonVar(std::string, std::string, bool, bool);

    const std::string PARAMTEMPL = R"(
                    "%s": {
                        "type": "%s"%s
                    })";
    const std::string PARAMGROUPTEMPL = R"(
                      "%s": {
                        "type": "array",
                        "xml": {
                            "name": "%s",
                            "wrapped": true
                        },
                        "items": {
                          "type": "object",
                          "properties": {
                            %s
                          }
                        }
                      },)";
    const std::string YAMLPARAMTEMPL = R"(
                                    "%s":
                                        "type": "%s"
                                        %s
            )";
    const std::string YAMLPARAMGROUPTEMPL = R"(
                                    "%s":
                                        "type": "array"
                                        "xml":
                                            "name": "%s"
                                            "wrapped": true
                                        "items":
                                            "type": "object"
                                            "properties":
                                                %s
            )";
};

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */