/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "MergeODFParser.h"

#include <cassert>
#include <iostream>
#include <fstream>
#include <string>

#include <Poco/FileStream.h>
#include <Poco/Tuple.h>
#include <Poco/DOM/DOMParser.h>
#include <Poco/DOM/Document.h>
#include <Poco/DOM/NodeList.h>
#include <Poco/DOM/DOMWriter.h>
#include <Poco/DOM/Text.h>
#include <Poco/Path.h>
#include <Poco/File.h>
#include <Poco/TemporaryFile.h>
#include <Poco/Base64Decoder.h>
#include <Poco/Dynamic/Var.h>
#include <Poco/Zip/Decompress.h>
#include <Poco/Zip/Compress.h>
#include <Poco/SAX/InputSource.h>

typedef Poco::Tuple<std::string, std::string> VarData;

/// 將 xml 內容存回 .xml 檔
void saveXmlBack(Poco::AutoPtr<Poco::XML::Document> docXML,
        std::string xmlfile)
{
    std::ostringstream ostrXML;
    Poco::XML::DOMWriter writer;
    writer.writeNode(ostrXML, docXML);
    const auto xml = ostrXML.str();

    Poco::File f(xmlfile);
    f.setSize(0);  // truncate

    Poco::FileOutputStream fos(xmlfile, std::ios::binary);
    fos << xml;
    fos.close();
}

/// check if number
bool isNumber(std::string s)
{
    std::size_t char_pos(0);
    // skip the whilespaces
    char_pos = s.find_first_not_of(' ');
    if (char_pos == s.size())
        return false;
    // check the significand
    if (s[char_pos] == '+' || s[char_pos] == '-')
        ++char_pos; // skip the sign if exist
    int n_nm, n_pt;
    for (n_nm = 0, n_pt = 0;
            std::isdigit(s[char_pos]) || s[char_pos] == '.';
            ++char_pos)
    {
        s[char_pos] == '.' ? ++n_pt : ++n_nm;
    }
    if (n_pt>1 || n_nm<1) // no more than one point, at least one digit
        return false;
    // skip the trailing whitespaces
    while (s[char_pos] == ' ') {
        ++ char_pos;
    }
    return char_pos == s.size(); // must reach the ending 0 of the string
}

/// 以檔名開啟
Parser::Parser()
    : picserial(0)
    , outAnotherJson(false)
    , outYaml(false)
{
}

Parser::~Parser()
{
    // 移除解壓縮的暫存目錄
    if (!extra2.empty())
    {
        Poco::File tempDir(extra2);
        if (tempDir.exists())
            tempDir.remove(true); // 完整移除該目錄及其下所有檔案
    }
}

/// set flags for /api /yaml or /json
void Parser::setOutputFlags(bool anotherJson, bool yaml)
{
    outAnotherJson = anotherJson;
    outYaml = yaml;
}

/// is text?
bool Parser::isText()
{
    return doctype == DocType::TEXT;
}

/// is spreadsheet?
bool Parser::isSpreadSheet()
{
    return doctype == DocType::SPREADSHEET;
}

/// mimetype used for http response's header
std::string Parser::getMimeType()
{
    switch (doctype)
    {
        case DocType::TEXT:
        default:
            return "application/vnd.oasis.opendocument.text";
        case DocType::SPREADSHEET:
            return "application/vnd.oasis.opendocument.spreadsheet";
    }
}

/// 將樣板檔解開
void Parser::extract(const std::string& templateFile)
{
    /* while (true)
    {
        extra2 = Poco::TemporaryFile::tempName();
        if(!Poco::File(extra2).exists())
            break;
    } */

    extra2 = Poco::TemporaryFile::tempName();

    std::ifstream inp(templateFile, std::ios::binary);

    assert (inp.good());
    Poco::Zip::Decompress dec(inp, extra2);
    dec.decompressAllFiles();
    assert (!dec.mapping().empty());

    zipfilepaths = dec.mapping();
    for (auto it = zipfilepaths.begin(); it != zipfilepaths.end(); ++it)
    {
        const auto fileName = it->second.toString();
        if (fileName == "content.xml")
            contentXmlFileName = extra2 + "/" + fileName;

        if (fileName == "META-INF/manifest.xml")
            metaFileName = extra2 + "/" + fileName;
    }
}

/// 傳回樣板變數的值
std::string Parser::varKeyValue(const std::string line,
        const std::string key)
{
    Poco::StringTokenizer tokens(line, ";", TOKENOPTS);
    for(size_t idx = 0; idx < tokens.count(); idx ++)
    {
        Poco::StringTokenizer keyval(tokens[idx], ":", TOKENOPTS);
        if (0 == Poco::icompare(keyval[0], key))
        {
            if (Poco::toLower(key) == "type")
            {
                if (0 == Poco::icompare(keyval[1], "image"))
                    return "file";
                if (0 == Poco::icompare(keyval[1], "enum"))
                    return "enum";
                if (0 == Poco::icompare(keyval[1], "auto"))
                    return "auto";
                if (0 == Poco::icompare(keyval[1], "boolean"))
                    return "boolean";
                if (0 == Poco::icompare(keyval[1], "float"))
                    return "float";
                if (0 == Poco::icompare(keyval[1], "percentage"))
                    return "percentage";
                if (0 == Poco::icompare(keyval[1], "currency"))
                    return "currency";
                if (0 == Poco::icompare(keyval[1], "date"))
                    return "date";
                if (0 == Poco::icompare(keyval[1], "time"))
                    return "time";
                if (0 == Poco::icompare(keyval[1], "Statistic"))
                    return "statistic";
                return "string";
            }
            if (keyval.count() == 2)
                return keyval[1];
            return "";  // split: 切完後若 aa: 後面沒字了
        }
    }
    return "";
}

// Type:Enum;Items:"男,女";Descript:"""
// Type:String;Description:""
// Type:String;Format:民國年/月/日
std::string Parser::parseJsonVar(std::string var,
        std::string vardata,
        bool anotherJson=false,
        bool yaml=false)
{
    std::string typevar = varKeyValue(vardata, "Type");
    std::string enumvar = varKeyValue(vardata, "Items");
    std::string descvar = varKeyValue(vardata, "Description");
    std::string formatvar = varKeyValue(vardata, "Format");
    std::string apihelpvar = varKeyValue(vardata, "ApiHelp");
    std::string databuf;

    bool first = true;
    if (typevar == "enum" && !enumvar.empty())
    {
        if (yaml)
        {
            Poco::replaceInPlace(enumvar, "\"", "");
            Poco::StringTokenizer tokens(enumvar, ",", TOKENOPTS);
            std::string enumvardata = "\"enum\": [";
            for(size_t idx = 0; idx < tokens.count(); idx ++)
            {
                const auto tok = tokens[idx];
                enumvardata += "\"" + tok + "\"";
                if (idx != tokens.count() - 1)
                    enumvardata += ",";
            }
            enumvardata += "]\n";
            databuf += enumvardata;
            first = false;
        }
        else
        {
            Poco::replaceInPlace(enumvar, "\"", "");
            Poco::StringTokenizer tokens(enumvar, ",", TOKENOPTS);
            std::string enumvardata = ",\n                        \"enum\":[";
            for(size_t idx = 0; idx < tokens.count(); idx ++)
            {
                const auto tok = tokens[idx];
                enumvardata += "\"" + tok + "\"";
                if (idx != tokens.count() - 1)
                    enumvardata += ",";
            }
            enumvardata += "]";
            databuf += enumvardata;
        }
    }
    //apihelpvar = "api說明";
    //descvar = "描述";
    if (!descvar.empty() || !apihelpvar.empty())
    {
        if (yaml)
        {
            Poco::replaceInPlace(descvar, "\"", "");
            if(first)
                databuf += "\"description\": \"";
            else
                databuf += "                                        \"description\": \"";
            if (!apihelpvar.empty())
                databuf += apihelpvar;
            if (!descvar.empty() && !apihelpvar.empty())
                databuf += "\n";
            if (!descvar.empty())
                databuf += descvar;
            databuf += "\"";
            databuf += "\n";
        }
        else
        {
            Poco::replaceInPlace(descvar, "\"", "");
            Poco::replaceInPlace(descvar, "\n", "<br />");  // @TODO: need?
            databuf += ",\n                        \"description\": \"";
            if (!apihelpvar.empty())
                databuf += apihelpvar;
            if (!descvar.empty() && !apihelpvar.empty())
                databuf += " / ";
            if (!descvar.empty())
                databuf +=  descvar;
            databuf += "\"";
        }
    }
    if (!formatvar.empty())
    {
        if (yaml)
        {
            Poco::replaceInPlace(formatvar, "\"", "");
            databuf += "                                        \"format\": \"";
            databuf += formatvar + "\"";
            databuf += "\n";
        }
        else
        {
            Poco::replaceInPlace(formatvar, "\"", "");
            databuf += ",\n                        \"format\": \""
                + formatvar + "\"";
        }
    }

    std::string realtype = varKeyValue(vardata, "Type");

    //auto jvalue = !formatvar.empty() ? formatvar : !descvar.empty() ? descvar : "";
    auto jjvalue = realtype;
    jjvalue += "  // ";
    if (!apihelpvar.empty())
        jjvalue += apihelpvar;
    if (!descvar.empty() && !apihelpvar.empty())
        jjvalue += " / ";
    if (!descvar.empty())
        jjvalue += descvar;

    std::string jvalue = "字串";
    if (realtype == "file")
    {
        jvalue = "array";
        //jjvalue = "base64 編碼";
        if (!yaml)
        {
            /*databuf +=
              ",\n                        \"description\": \"上傳圖片說明\",";*/
            databuf += R"MULTILINE(,
                        "items": {
                        "type": "string",
                        "format": "binary"
                      })MULTILINE";
        }
        else
        {
            databuf += "                \"items\":";
            databuf += "\n";
            databuf += "                  \"type\": \"";
            databuf += "string\"";
            databuf += "\n";
            databuf += "                  \"format\": \"";
            databuf += "binary\"";
            databuf += "\n";
        }
    }

    if (realtype == "string")
    {
        jvalue = "string";
        //jjvalue = apihelpvar.empty() ? "字串" : apihelpvar;
    }
    if (realtype == "auto")
    {
        jvalue = "string";
        //jjvalue = apihelpvar.empty() ? "字串" : apihelpvar;
        jjvalue = "string or float";
        jjvalue += "  // ";  // @TODO: 與上段重複
        if (!apihelpvar.empty())
            jjvalue += apihelpvar;
        if (!descvar.empty() && !apihelpvar.empty())
            jjvalue += " / ";
        if (!descvar.empty())
            jjvalue += descvar;
    }
    if (realtype == "float")
    {
        jvalue = "number";
        //jjvalue = apihelpvar.empty() ? "123.45" : apihelpvar;
    }
    if (realtype == "enum")
    {
        jvalue = "string";
        //jjvalue = apihelpvar.empty() ? "1" : apihelpvar;
    }
    if (realtype == "boolean")
    {
        jvalue = "boolean";
        //jjvalue = apihelpvar.empty() ? "true 或 false" : apihelpvar;
    }
    if (realtype == "date")
    {
        jvalue = "string";
        //jjvalue = apihelpvar.empty() ? "2018-07-25" : apihelpvar;
    }
    if (realtype == "time")
    {
        jvalue = "string";
        //jjvalue = apihelpvar.empty() ?
        //                "PT09H25M00S(為：09:25:00)" : apihelpvar;
    }
    if (realtype == "percentage")
    {
        jvalue = "number";
        //jjvalue = apihelpvar.empty() ? "0.123" : apihelpvar;
    }
    if (realtype == "currency")
    {
        jvalue = "integer";
        //jjvalue = apihelpvar.empty() ? "100000" : apihelpvar;
    }
    if (realtype == "statistic")
    {
        jvalue = "string";
    }

    if (anotherJson)
        return "\"" + var + "\": " + "\"" + jjvalue + "\"";

    if (outYaml)
        return Poco::format(YAMLPARAMTEMPL, var, jvalue, databuf);
    else
        return Poco::format(PARAMTEMPL, var, jvalue, databuf);
}

/// get doc type
void Parser::detectDocType()
{
    auto path = "//office:body/office:text";
    auto found = static_cast<Poco::XML::Node*>(docXML->getNodeByPath(path));
    if (found)
    {
        doctype = DocType::TEXT;
    }
    path = "//office:body/office:spreadsheet";
    found = static_cast<Poco::XML::Node*>(docXML->getNodeByPath(path));
    if (found)
    {
        doctype = DocType::SPREADSHEET;
    }
}

/// translate enum and boolean value
std::string Parser::parseEnumValue(std::string type,
        std::string enumvar,
        std::string value)
{
    if (type == "enum" && isNumber(value))
    {
        Poco::replaceInPlace(enumvar, "\"", "");
        Poco::StringTokenizer tokens(enumvar, ",", TOKENOPTS);
        int enumIdx = std::stoi(value)-1;
        if (enumIdx >= 0 && (unsigned)enumIdx < tokens.count())
        {
            value = tokens[enumIdx];
        }
    }
    if (type == "boolean")  // True、Yes、1
    {
        Poco::replaceInPlace(enumvar, "\"", "");
        Poco::StringTokenizer tokens(enumvar, ",", TOKENOPTS);
        int enumIdx = ("1" == value ||
                0 == Poco::icompare(value, "true") ||
                0 == Poco::icompare(value, "yes")) ? 0 : 1;
        value = tokens[enumIdx];
    }
    return value;
}

/// meta-inf: xxx-template -> xxx
/// for bug: excel/word 不能開啟 xxx-template 的文件
std::string Parser::replaceMetaMimeType(std::string attr)
{
    Poco::replaceInPlace(attr,
            "application/vnd.oasis.opendocument.text-template",
            "application/vnd.oasis.opendocument.text");
    Poco::replaceInPlace(attr,
            "application/vnd.oasis.opendocument.spreadsheet-template",
            "application/vnd.oasis.opendocument.spreadsheet");
    return attr;
}

/// for bug: excel/word 不能開啟 xxx-template 的文件
void Parser::updateMetaInfo()
{
    /// meta-inf file
    Poco::XML::InputSource inputSrc(metaFileName);
    Poco::XML::DOMParser parser;
    auto docXmlMeta = parser.parse(&inputSrc);
    auto listNodesMeta =
        docXmlMeta->getElementsByTagName("manifest:file-entry");

    for (unsigned long it = 0; it < listNodesMeta->length(); ++it)
    {
        auto elm = static_cast<Poco::XML::Element*>(listNodesMeta->item(it));
        if (elm->getAttribute("manifest:full-path") == "/")
        {
            auto attr = elm->getAttribute("manifest:media-type");
            elm->setAttribute("manifest:media-type",
                    replaceMetaMimeType(attr));
        }
    }
    saveXmlBack(docXmlMeta, metaFileName);

    /// mimetype file
    auto mimeFile = extra2 + "/mimetype";
    Poco::FileInputStream istr(mimeFile);
    std::string mime;
    istr >> mime;
    istr.close();

    mime = replaceMetaMimeType(mime);
    Poco::File f(mimeFile);
    f.setSize(0);  // truncate

    Poco::FileOutputStream fos(mimeFile, std::ios::binary);
    fos << mime;
    fos.close();
}

/// write picture info to meta file
void Parser::updatePic2MetaXml()
{
    Poco::XML::InputSource inputSrc(metaFileName);
    Poco::XML::DOMParser parser;
    //parser.setFeature(XMLReader::FEATURE_NAMESPACE_PREFIXES, false);
    auto docXmlMeta = parser.parse(&inputSrc);
    auto listNodesMeta =
        docXmlMeta->getElementsByTagName("manifest:manifest");
    auto pElm = docXmlMeta->createElement("manifest:file-entry");
    pElm->setAttribute("manifest:full-path",
            "Pictures/" + std::to_string(picserial));
    pElm->setAttribute("manifest:media-type", "");
    static_cast<Poco::XML::Element*>(listNodesMeta->item(0))->appendChild(pElm);

    saveXmlBack(docXmlMeta, metaFileName);
}

/// zip it
std::string Parser::zipback()
{
    updateMetaInfo();
    saveXmlBack(docXML, contentXmlFileName);

    // zip
    const std::string zip2 = extra2 + (isText() ? ".odt" : ".ods");

    std::ofstream out(zip2, std::ios::binary);
    Poco::Zip::Compress c(out, true);

    c.addRecursive(extra2);
    c.close();
    return zip2;
}

/// get json
std::string Parser::jsonVars()
{
    jsonvars = "";

    auto allVar = scanVarPtr();
    std::list<Poco::XML::Element*> singleVar = allVar[0];
    std::list<Poco::XML::Element*> groupVar = allVar[1];

    std::string Var_Tag_Property;
    if(isText())
        Var_Tag_Property = "text:description";
    else if(isSpreadSheet())
        Var_Tag_Property = "office:target-frame-name";

    std::string VAR_TAG;
    if(isText())
        VAR_TAG = "text:placeholder";
    else if(isSpreadSheet())
        VAR_TAG = "text:a";

    std::list<std::string> singleList;
    for (auto it = singleVar.begin(); it!=singleVar.end(); it++)
    {
        auto elm = *it;
        auto varName = elm->innerText();
        if(isText())
            varName = varName.substr(1, varName.size()-2);
        auto checkExist = std::find(singleList.begin(), singleList.end(), varName);
        if(checkExist != singleList.end())
            continue;
        jsonvars += parseJsonVar(varName, elm->getAttribute(Var_Tag_Property)) + ",";
        singleList.push_back(varName);
    }
    std::list<std::string> groupList;
    for (auto it = groupVar.begin(); it!=groupVar.end(); it++)
    {
        auto checkGrpExist = std::find(groupList.begin(), groupList.end(), (*it)->getAttribute("grpname"));
        if(checkGrpExist != groupList.end())
            continue;
        groupList.push_back((*it)->getAttribute("grpname"));

        auto rowVar = (*it)->getElementsByTagName(VAR_TAG);
        int childLen = rowVar->length();
        std::string cells = "";
        std::string grpname = (*it)->getAttribute("grpname");
        std::list<std::string> childVarList;
        for (int i=0; i<childLen; i++)
        {
            auto elm = static_cast<Poco::XML::Element*>(rowVar->item(i));
            auto varName = elm->innerText();
            if(isText())
                varName = varName.substr(1, varName.size()-2);
            auto checkVarExist = std::find(childVarList.begin(), childVarList.end(), varName);
            if(checkVarExist != childVarList.end())
                continue;
            childVarList.push_back(varName);
            cells += parseJsonVar(varName, elm->getAttribute(Var_Tag_Property));
            if ((i+1)<childLen)
                cells += ",";
        }
        jsonvars += Poco::format(PARAMGROUPTEMPL, grpname, grpname, cells);
    }
    jsonvars = jsonvars.substr(0, jsonvars.length() - 1);
    return jsonvars;
}

// get json for another
std::string Parser::jjsonVars()
{
    jjsonvars = "";

    auto allVar = scanVarPtr();
    std::list<Poco::XML::Element*> singleVar = allVar[0];
    std::list<Poco::XML::Element*> groupVar = allVar[1];

    std::string Var_Tag_Property;
    if(isText())
        Var_Tag_Property = "text:description";
    else if(isSpreadSheet())
        Var_Tag_Property = "office:target-frame-name";

    std::string VAR_TAG;
    if(isText())
        VAR_TAG = "text:placeholder";
    else if(isSpreadSheet())
        VAR_TAG = "text:a";

    std::list<std::string> singleList;
    for (auto it = singleVar.begin(); it!=singleVar.end(); it++)
    {
        auto elm = *it;
        auto varName = elm->innerText();
        if(isText())
            varName = varName.substr(1, varName.size()-2);
        auto checkExist = std::find(singleList.begin(), singleList.end(), varName);
        if (checkExist != singleList.end())
            continue;
        singleList.push_back(varName);
        jjsonvars += parseJsonVar(varName, elm->getAttribute(Var_Tag_Property), true) + ",<br />";
    }

    std::list<std::string> groupList;
    for (auto it = groupVar.begin(); it!=groupVar.end(); it++)
    {
        auto checkGrpExist = std::find(groupList.begin(), groupList.end(), (*it)->getAttribute("grpname"));
        if(checkGrpExist != groupList.end())
            continue;
        groupList.push_back((*it)->getAttribute("grpname"));

        auto rowVar = (*it)->getElementsByTagName(VAR_TAG);
        int childLen = rowVar->length();
        std::string grpname = (*it)->getAttribute("grpname");

        jjsonvars += "&nbsp;&nbsp;&nbsp;&nbsp;\"" + grpname + "\":[<br />";
        jjsonvars += "&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;{";

        std::list<std::string> childVarList;
        for (int i=0; i<childLen; i++)
        {
            auto elm = static_cast<Poco::XML::Element*>(rowVar->item(i));
            auto varName = elm->innerText();
            if(isText())
                varName = varName.substr(1,varName.size()-2);
            auto checkVarExist = std::find(childVarList.begin(), childVarList.end(), varName);
            if(checkVarExist != childVarList.end())
                continue;
            childVarList.push_back(varName);

            jjsonvars += parseJsonVar(varName, elm->getAttribute(Var_Tag_Property), true);
            if ((i+1) != childLen)
                jjsonvars += ",";
        }
        jjsonvars += "}";
        jjsonvars += "<br />&nbsp;&nbsp;&nbsp;&nbsp;]";



        auto kk = it;
        if ((kk++) != (groupVar.end()))
            jjsonvars += ",";

        jjsonvars += "<br />";
    }
    if (jjsonvars.substr(jjsonvars.length() - 7, 7) == ",<br />")
    {
        jjsonvars = jjsonvars.substr(0, jjsonvars.length() - 7);
        jjsonvars += "<br />";
    }
    return jjsonvars;
}

// get yaml
std::string Parser::yamlVars()
{
    yamlvars = "";

    auto allVar = scanVarPtr();
    std::list<Poco::XML::Element*> singleVar = allVar[0];
    std::list<Poco::XML::Element*> groupVar = allVar[1];

    std::string Var_Tag_Property;
    if(isText())
        Var_Tag_Property = "text:description";
    else if(isSpreadSheet())
        Var_Tag_Property = "office:target-frame-name";

    std::string VAR_TAG;
    if(isText())
        VAR_TAG = "text:placeholder";
    else if(isSpreadSheet())
        VAR_TAG = "text:a";


    std::list<std::string> singleList;
    for (auto it = singleVar.begin(); it!=singleVar.end(); it++)
    {
        auto elm = *it;
        auto varName = elm->innerText();
        if(isText())
            varName = varName.substr(1, varName.size()-2);
        auto checkExist = std::find(singleList.begin(), singleList.end(), varName);
        if(checkExist != singleList.end())
            continue;

        singleList.push_back(varName);
        yamlvars += parseJsonVar(varName, elm->getAttribute(Var_Tag_Property), false, true);
    }

    std::list<std::string> groupList;
    for (auto it = groupVar.begin(); it!=groupVar.end(); it++)
    {
        auto checkGrpExist = std::find(groupList.begin(), groupList.end(), (*it)->getAttribute("grpname"));
        if(checkGrpExist != groupList.end())
            continue;
        groupList.push_back((*it)->getAttribute("grpname"));
        auto rowVar = (*it)->getElementsByTagName(VAR_TAG);
        int childLen = rowVar->length();
        std::string grpname = (*it)->getAttribute("grpname");
        std::string cells = "";

        std::list<std::string> childVarList;
        for (int i=0; i<childLen; i++)
        {
            auto elm = static_cast<Poco::XML::Element*>(rowVar->item(i));
            auto varName = elm->innerText();
            if(isText())
                varName = varName.substr(1, varName.size()-2);
            auto checkVarExist = std::find(childVarList.begin(), childVarList.end(), varName);
            if(checkVarExist != childVarList.end())
                continue;
            childVarList.push_back(varName);

            std::string var = parseJsonVar(varName, elm->getAttribute(Var_Tag_Property), outAnotherJson, outYaml);
            std::string newSpaceVar;

            /// 補上空白 = ident 符合 array
            Poco::StringTokenizer tokens(var, "\n", Poco::StringTokenizer::TOK_IGNORE_EMPTY);
            for(size_t idx = 0; idx < tokens.count(); idx ++)
            {
                newSpaceVar += "              " + tokens[idx] + "\n";
            }
            cells += newSpaceVar;
        }
        yamlvars += Poco::format(YAMLPARAMGROUPTEMPL, grpname, grpname, cells);
    }
    return yamlvars;
}

// 取出單一變數與群組變數的記憶體位置
std::vector<std::list<Poco::XML::Element*>> Parser::scanVarPtr()
{
    // Load XML to program
    Poco::XML::InputSource inputSrc(contentXmlFileName);
    Poco::XML::DOMParser parser;
    parser.setFeature(Poco::XML::XMLReader::FEATURE_NAMESPACES, false);
    parser.setFeature(Poco::XML::XMLReader::FEATURE_NAMESPACE_PREFIXES, true);
    docXML = parser.parse(&inputSrc);

    Poco::AutoPtr<Poco::XML::NodeList> listNodes;
    std::list <VarData> listvars;
    std::list <Poco::XML::Element*> singleVar;
    std::list <Poco::XML::Element*> groupVar;
    std::vector <std::list<Poco::XML::Element*>> result;

    detectDocType();

    if (isText())
    {

        // Scan All Var Pointer save into list
        listNodes = docXML->getElementsByTagName("text:placeholder");
        int totalVar = listNodes->length();
        for (int idx=0; idx < totalVar; idx++)
        {
            Poco::XML::Element* currentNode = static_cast<Poco::XML::Element*>(listNodes->item(idx));
            auto Parent_1 = static_cast<Poco::XML::Element*>(currentNode->parentNode());
            auto Parent_2 = Parent_1->parentNode();
            while(true){
                std::string nodeName = Parent_2->nodeName();
                if(nodeName == "office:text" || nodeName == "table:table-cell")
                {
                    break;
                }
                Parent_2 = Parent_2->parentNode();
            }
            auto Parent_3 = static_cast<Poco::XML::Element*>(Parent_2->parentNode());
            if(Parent_2->nodeName() != "table:table-cell")
            {
                singleVar.push_back(currentNode);
            }
            else
            {
                auto grpNodeList = Parent_3->getElementsByTagName("office:annotation");
                int grpLen = grpNodeList->length();
                if (grpLen == 0)
                {
                    singleVar.push_back(currentNode);
                }
                else
                {
                    // If there are different office:annotation name, only take the first grpname as target
                    std::string grpname = grpNodeList->item(0)->lastChild()->innerText();
                    Parent_3->setAttribute("grpname", grpname);
                    auto checkDuplicate = std::find(groupVar.begin(), groupVar.end(), Parent_3);
                    if (checkDuplicate == groupVar.end())
                        groupVar.push_back(Parent_3);
                }

            }
        }

        // 刪掉 grp tag
        auto grpNodeList = docXML->getElementsByTagName("office:annotation");
        int grpLen = grpNodeList->length();
        for(auto tmp_grp_len = 0; tmp_grp_len < grpLen; tmp_grp_len++)
        {
            auto grpNode = grpNodeList->item(0);
            grpNode->parentNode()->removeChild(grpNode);
        }
        grpNodeList = docXML->getElementsByTagName("office:annotation-end");
        grpLen = grpNodeList->length();
        for(auto tmp_grp_len = 0; tmp_grp_len < grpLen; tmp_grp_len++)
        {
            auto grpNode = grpNodeList->item(0);
            grpNode->parentNode()->removeChild(grpNode);
        }
    }
    if (isSpreadSheet())
    {
        // Scan All Var Pointer save into list
        listNodes = docXML->getElementsByTagName("text:a");
        int totalVar = listNodes->length();
        for (int idx=0; idx < totalVar; idx++)
        {
            Poco::XML::Element* currentNode = static_cast<Poco::XML::Element*>(listNodes->item(idx));
            std::string vardata  = currentNode->getAttribute("office:target-frame-name");
            std::string type     = varKeyValue(vardata, "type");
            auto Parent_1 = static_cast<Poco::XML::Element*>(currentNode->parentNode());
            auto Parent_2 = static_cast<Poco::XML::Element*>(Parent_1->parentNode());
            while(true){
                std::string nodeName = Parent_2->nodeName();
                if(nodeName == "table:table" || nodeName == "table:table-row-group")
                {
                    break;
                }
                Parent_2 = static_cast<Poco::XML::Element*>(Parent_2->parentNode());
            }
            Parent_2 = static_cast<Poco::XML::Element*> (Parent_2);
            // 如果是 SC 的範本精靈把群組去掉後會保留 table-row-group 所以要雙重檢測
            if(Parent_2->nodeName() == "table:table")
            {
                singleVar.push_back(currentNode);
            }
            // 儘管是群組中的統計變數也要拉出來個別處理，不然在 setGroupVar 無法進行全域的 jsonData 掃描
            else if (type == "statistic")
            {
                singleVar.push_back(currentNode);
            }
            else
            {
                auto grpNodeList = Parent_2->getElementsByTagName("office:annotation");
                int grpLen = grpNodeList->length();
                if (grpLen == 0)
                {
                    singleVar.push_back(currentNode);
                }
                else
                {
                    // If there are different office:annotation name, only take the first grpname as target
                    std::string grpname = grpNodeList->item(0)->lastChild()->innerText();
                    //Ensure put attr grpname in the table:table-row not in table:table-row-group!
                    Parent_2 = static_cast<Poco::XML::Element*> (Parent_2->firstChild());
                    while(true)
                    {
                        if (Parent_2->nodeName()=="table:table-row")
                            break;
                        Parent_2 = static_cast<Poco::XML::Element*> (Parent_2->firstChild());
                    }
                    Parent_2->setAttribute("grpname", grpname);
                    auto checkDuplicate = std::find(groupVar.begin(), groupVar.end(), Parent_2);
                    if (checkDuplicate == groupVar.end())
                        groupVar.push_back(Parent_2);
                }

            }
        }

        // 刪掉 grp tag
        auto grpNodeList = docXML->getElementsByTagName("office:annotation");
        int grpLen = grpNodeList->length();
        for(auto tmp_grp_len = 0; tmp_grp_len < grpLen; tmp_grp_len++)
        {
            auto grpNode = grpNodeList->item(0);
            grpNode->parentNode()->removeChild(grpNode);
        }
        grpNodeList = docXML->getElementsByTagName("office:annotation-end");
        grpLen = grpNodeList->length();
        for(auto tmp_grp_len = 0; tmp_grp_len < grpLen; tmp_grp_len++)
        {
            auto grpNode = grpNodeList->item(0);
            grpNode->parentNode()->removeChild(grpNode);
        }
    }

    result.push_back(singleVar);
    result.push_back(groupVar);
    return result;
}

// Insert value into group Variable
void Parser::setGroupVar(Poco::JSON::Object::Ptr jsonData, std::list<Poco::XML::Element*> &groupVar)
{
    // Text & SC 的變數 xml tag 有所不同
    std::string VAR_TAG;
    if(isText())
        VAR_TAG = "text:placeholder";
    else if(isSpreadSheet())
        VAR_TAG = "text:a";

    for (auto it = groupVar.begin(); it!=groupVar.end(); it++)
    {
        Poco::XML::Element* row = *it;
        Poco::XML::Node* currentRow = row;
        Poco::XML::Node* realBaseRow = currentRow;
        Poco::XML::Node *nextRow;
        Poco::XML::Node *rootTable;
        Poco::XML::Node* pTbRow ;

        // 針對 Array 的存取目前我們只能作到透過 Var 先判定一次資料是否存在，然後在轉成 Array，如果直接針對 Array 取值會導致無法判斷是否為空的 Array
        Poco::JSON::Array::Ptr arr;
        int lines = 0;
        std::string grpname = row->getAttribute("grpname");
        if (jsonData->has(grpname))
        {
            Poco::Dynamic::Var tmpData = jsonData->get(grpname);
            if(tmpData.isArray())
            {
                arr = tmpData.extract<Poco::JSON::Array::Ptr>();
                lines = arr->size();
            }
            else
            {
                row->parentNode()->removeChild(row);
                continue;
            }
        }
        else
        {
            row->parentNode()->removeChild(row);
            continue;
        }

        /* 初始化「樣板列」的過程 Text & SC 的 xml 結構有所差異
        */

        Poco::XML::Node* initRow = nullptr;
        if(isSpreadSheet())
        {
            // 初始化樣板列:
            // 1.移除非變數的欄位之內含儲存格內容以及儲存格之特性
            // 2.移除統計變數 (只去除第一行以後的)
            initRow = realBaseRow->cloneNode(true);
            auto child = static_cast<Poco::XML::Element*>(initRow->firstChild());//table:table-cell
            while(child)
            {
                if(child->getElementsByTagName("text:a")->length()==0)
                {
                    if (child->getElementsByTagName("text:p")->length()!=0)
                    {
                        auto target = static_cast<Poco::XML::Element*>(child->firstChild());
                        while(target)
                        {
                            if(target->nodeName()=="text:p")
                            {
                                child->removeChild(target);
                            }

                            target = static_cast<Poco::XML::Element*>(target->nextSibling());
                        }

                    }
                    // 清除 table:table-cell 的 attribute
                    child->removeAttribute("office:value");
                    child->removeAttribute("office:value-type");
                    child->removeAttribute("calcext:value-type");
                    child->removeAttribute("table:formula");
                }
                else
                {
                    // 移除統計變數
                    // 前端設計工具限定一個儲存格只有一個變數
                    auto variableList = child->getElementsByTagName("text:a");
                    Poco::XML::Element* target = static_cast<Poco::XML::Element*> (variableList->item(0));
                    auto vardata =  target->getAttribute("office:target-frame-name");
                    auto type = varKeyValue(vardata, "type");
                    if(type == "statistic")
                    {
                        child->removeChild(target->parentNode());
                        child->removeAttribute("office:value");
                        child->removeAttribute("office:value-type");
                        child->removeAttribute("calcext:value-type");
                    }
                }
                child = static_cast<Poco::XML::Element*>(child->nextSibling());
            }
            // 擴增跨列的行數
            Poco::XML::Node* targetNode = realBaseRow;
            while(targetNode->nodeName() != "table:table-row-group")
                targetNode = targetNode->parentNode();

            Poco::XML::Element* spanRow;
            if(targetNode->previousSibling()!=NULL)
                spanRow = static_cast<Poco::XML::Element*> (targetNode->previousSibling()->firstChild());
            else
                spanRow = static_cast<Poco::XML::Element*> (targetNode);

            while(spanRow)
            {
                if (spanRow->hasAttribute("table:number-rows-spanned"))
                    spanRow->setAttribute("table:number-rows-spanned", std::to_string(lines+1));
                spanRow = static_cast<Poco::XML::Element*> (spanRow->nextSibling());
            }
        }
        else if (isText())
        {
            // 初始化整列: 主要是去除非編號(1.\n 2. ...etc)的欄位之數值
            initRow = realBaseRow->cloneNode(true);
            auto child = static_cast<Poco::XML::Element*>(initRow->firstChild());
            while(child)
            {
                if(child->getElementsByTagName(VAR_TAG)->length()==0)
                {
                    if (child->getElementsByTagName("text:list")->length()==0)
                        if(child->childNodes()->length()!=0)
                            child->removeChild(child->getElementsByTagName("text:p")->item(0));
                }

                child = static_cast<Poco::XML::Element*>(child->nextSibling());
            }
            // 擴增跨列的行數
            auto spanRow = static_cast<Poco::XML::Element*> (realBaseRow->previousSibling()->firstChild());
            while(spanRow)
            {
                if (spanRow->hasAttribute("table:number-rows-spanned"))
                    spanRow->setAttribute("table:number-rows-spanned", std::to_string(lines+1));
                spanRow = static_cast<Poco::XML::Element*> (spanRow->nextSibling());
            }
        }

        /// 列群組：add rows, then set form var data
        for (int times = 0; times < lines; times ++)
        {
            if (times==0)
                //保留第一行的格式不變
                pTbRow = realBaseRow->cloneNode(true);
            else
                pTbRow = initRow->cloneNode(true);
            // insert new row to the table
            nextRow = currentRow->nextSibling();
            rootTable = currentRow->parentNode();
            rootTable->insertBefore(pTbRow, nextRow);
            currentRow = pTbRow;

            /// put var values into group
            auto rowChildVar = (static_cast<Poco::XML::Element*>(pTbRow))->getElementsByTagName(VAR_TAG);
            int childLen = rowChildVar->length();
            std::list<Poco::XML::Element*> varList;
            for (int i=0; i<childLen; i++)
            {
                varList.push_back(static_cast<Poco::XML::Element*> (rowChildVar->item(i)));
            }

            auto arrData = arr->getObject(times);
            if(times==0)
            {
                for(auto each=varList.begin(); each!=varList.end(); each++)
                {
                    std::string eachName = (*each)->innerText();

                    Poco::Dynamic::Var value;
                    if (isText())
                        value = jsonData->get(eachName.substr(1, eachName.size()-2));
                    else if(isSpreadSheet())
                        value = jsonData->get(eachName);

                    if (!value.isEmpty())
                    {
                        arrData->set(eachName, value);
                    }
                }
            }
            setSingleVar(arr->getObject(times), varList);

        }
        // Remove template Row
        row->parentNode()->removeChild(row);
    }
}

// Insert into single Variable
void Parser::setSingleVar(Poco::JSON::Object::Ptr jsonData, std::list<Poco::XML::Element*> &singleVar)
{
    /* 函數說明
     *  jsonData 的來源有可能是 request or setGroupVar's jsonData' Array 而來
     *  singleVar 跟 jsonData 來源類似
     */

    //初始化 Text & SC 的 Tag 區別
    //  1. 只有 tag 內含的 property 需要區分
    std::string Var_Tag_Property;
    if(isText())
        Var_Tag_Property = "text:description";
    else if(isSpreadSheet())
        Var_Tag_Property = "office:target-frame-name";

    for (auto it = singleVar.begin(); it!=singleVar.end(); it++)
    {
        Poco::XML::Element* elm = *it;
        auto vardata = elm->getAttribute(Var_Tag_Property);
        std::string type = varKeyValue(vardata, "type");

        // 模板變數的類型需要針對 file 特別處理，因為 file 需要把檔案寫在 extract 的資料夾內部
        if (type != "file" and type != "statistic")
        {
            std::string key = elm->innerText();
            Poco::Dynamic::Var value;
            if (isText())
                value = jsonData->get(key.substr(1,key.size()-2));
            else if(isSpreadSheet())
                value = jsonData->get(key);

            if (value.isEmpty())
            {
                elm->parentNode()->removeChild(elm);
                continue;
            }

            // 根據 json 拿到的 value 作數值轉換 (boolean, list)
            auto enumvar = varKeyValue(vardata, "Items");
            auto format = varKeyValue(vardata, "Format");
            value = parseEnumValue(type, enumvar, value.toString());


            // 依照不同型別進行個別處理
            if (type == "auto" && isNumber(value) && isSpreadSheet())
            {
                auto meta = static_cast<Poco::XML::Element*>(elm->parentNode()->parentNode());
                auto pVal = docXML->createTextNode(value);
                elm->parentNode()->replaceChild(pVal, elm);
                type = "float";
                meta->setAttribute("office:value", value);
                meta->setAttribute("office:value-type", type);
                meta->setAttribute("calcext:value-type", type);
            }
            else if ( (type == "float" || type == "percentage" ||
                    type == "currency" || type == "date" ||
                    type == "time" )
                    && isSpreadSheet())
            {

                auto meta = static_cast<Poco::XML::Element*>(elm->parentNode()->parentNode());
                auto pVal = docXML->createTextNode(value);
                elm->parentNode()->replaceChild(pVal, elm);
                meta->setAttribute("office:value-type", type);
                meta->setAttribute("calcext:value-type", type);
                auto officeValue = "office:" + format;
                meta->setAttribute(officeValue, value);
            }
            else {
                // Writer 一定跑到這裡來
                auto pVal = docXML->createTextNode(value);
                elm->parentNode()->replaceChild(pVal, elm);
            }
        }
        else if (type == "statistic")
        {
            std::string grpname = varKeyValue(vardata, "groupname");
            std::string column = varKeyValue(vardata, "column");
            std::string method = varKeyValue(vardata, "method");
            std::string targetVariable = varKeyValue(vardata, "Items");

            Poco::StringTokenizer tokens(column, ".", TOKENOPTS);
            std::string cell = tokens[1];
            Poco::StringTokenizer addr(cell, "$", TOKENOPTS);
            // addr[0] 是 欄位代號 :ex A
            // addr[1] 是 列位編號 :ex 1
            std::string cellAddr = addr[0] + addr[1];
            column = addr[0];

            Poco::JSON::Array::Ptr arr;
            int lines;
            if (jsonData->has(grpname))
            {
                Poco::Dynamic::Var tmpData = jsonData->get(grpname);
                if(tmpData.isArray())
                {
                    arr = tmpData.extract<Poco::JSON::Array::Ptr>();
                    lines = arr->size();
                }
                else
                {
                    elm->parentNode()->removeChild(elm);
                    continue;
                }
            }
            else
            {
                elm->parentNode()->removeChild(elm);
                continue;
            }
            auto newElm = docXML->createElement("table:table-cell");
            if (method == "總和")
                method = "SUM";
            if (method == "最大值")
                method = "MAX";
            if (method == "最小值")
                method = "MIN";
            if (method == "中位數")
                method = "MEDIAN";
            if (method == "計數")
                method = "COUNT";
            if (method == "平均")
                method = "AVERAGE";
            std::string formula = "of:="+ method +"([."+cellAddr+":."+column+std::to_string(std::stoi(addr[1])+lines-1)+"])";
            newElm->setAttribute("table:formula", formula);
            newElm->setAttribute("office:value-type", "float");
            newElm->setAttribute("calcext:value-type", "float");
            auto pCell = elm->parentNode()->parentNode();
            pCell->parentNode()->replaceChild(newElm, pCell);
        }
        else if (type == "file")
        {
            //Write file into extract directory
            std::string varname = elm->innerText();
            Poco::Dynamic::Var value;
            if (isText())
            {
                varname = varname.substr(1, varname.size()-2);
                value = jsonData->get(varname);
            }
            else if(isSpreadSheet())
                value = jsonData->get(varname);

            if (value.isEmpty())
            {
                elm->parentNode()->removeChild(elm);
                continue;
            }

            auto enumvar = varKeyValue(vardata, "Items");
            value = parseEnumValue(type, enumvar, value);


            auto tempPath = Poco::Path::forDirectory(Poco::TemporaryFile::tempName() + "/");
            Poco::File(tempPath).createDirectories();
            const Poco::Path filenameParam(varname);
            tempPath.setFileName(filenameParam.getFileName());
            auto _filename = tempPath.toString();

            try
            {
                // Write b64encode data to image
                std::stringstream ss;
                ss << value.toString();
                Poco::Base64Decoder b64in(ss);
                std::ofstream ofs(_filename);
                std::copy(std::istreambuf_iterator<char>(b64in),
                        std::istreambuf_iterator<char>(),
                        std::ostreambuf_iterator<char>(ofs));
            }
            catch (Poco::Exception& e)
            {
                std::cerr << e.displayText() << std::endl;
            }


            // Write file info to Xml tag
            updatePic2MetaXml();

            if (isText())
            {
                auto desc = elm->getAttribute(Var_Tag_Property);

                // image size
                auto imageSize = varKeyValue(desc, "Size");
                std::string width = "2.5cm", height = "1.5cm";
                if (!imageSize.empty())
                {
                    Poco::StringTokenizer token(imageSize, "x", TOKENOPTS);
                    width = token[0] + "cm";
                    height = token[1] + "cm";
                }

                auto pElm = docXML->createElement("draw:frame");
                pElm->setAttribute("draw:style-name", "fr1");
                pElm->setAttribute("draw:name", "Image1");
                pElm->setAttribute("text:anchor-type", "as-char");
                pElm->setAttribute("svg:width", width);
                pElm->setAttribute("svg:height", height);
                pElm->setAttribute("draw:z-index", "1");

                auto pChildElm = docXML->createElement("draw:image");
                pChildElm->setAttribute("xlink:href",
                        "Pictures/" + std::to_string(picserial));
                pChildElm->setAttribute("xlink:type", "simple");
                pChildElm->setAttribute("xlink:show", "embed");
                pChildElm->setAttribute("xlink:actuate", "onLoad");
                pChildElm->setAttribute("loext:mime-type", "image/png");
                pElm->appendChild(pChildElm);

                auto node = elm->parentNode();
                node->replaceChild(pElm, elm);

                const auto picdir = extra2 + "/Pictures";
                Poco::File(picdir).createDirectory();
                const auto picfilepath = picdir + "/" +
                    std::to_string(picserial);
                Poco::File(_filename).copyTo(picfilepath);
                picserial ++;
            }
            else if (isSpreadSheet())
            {
                auto desc = elm->getAttribute(Var_Tag_Property);

                // image size
                auto imageSize = varKeyValue(desc, "Size");
                std::string width = "2.5cm", height = "1.5cm";
                if (!imageSize.empty())
                {
                    Poco::StringTokenizer token(imageSize, "x", TOKENOPTS);
                    width = token[0] + "cm";
                    height = token[1] + "cm";
                }

                auto pElm = docXML->createElement("draw:frame");
                pElm->setAttribute("draw:style-name", "gr1");
                pElm->setAttribute("draw:name", "Image1");
                pElm->setAttribute("svg:width", width);
                pElm->setAttribute("svg:height", height);
                pElm->setAttribute("draw:z-index", "1");

                auto pChildElm = docXML->createElement("draw:image");
                pChildElm->setAttribute("xlink:href", "Pictures/" + std::to_string(picserial));
                pChildElm->setAttribute("xlink:type", "simple");
                pChildElm->setAttribute("xlink:show", "embed");
                pChildElm->setAttribute("xlink:actuate", "onLoad");
                pChildElm->setAttribute("loext:mime-type", "image/png");
                pElm->appendChild(pChildElm);

                // 直接替換掉整個儲存格，避免遺留不必要的特性
                auto newCell = docXML->createElement("table:table-cell");
                auto oldCell = elm->parentNode()->parentNode();
                auto node = elm->parentNode()->parentNode()->parentNode();

                newCell->appendChild(pElm);
                node->replaceChild(newCell, oldCell);

                const auto picdir = extra2 + "/Pictures";
                Poco::File(picdir).createDirectory();
                const auto picfilepath = picdir + "/" +
                    std::to_string(picserial);
                Poco::File(_filename).copyTo(picfilepath);
                picserial ++;
            }
        }
    }
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
