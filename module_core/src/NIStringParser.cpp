#include "NIStringParser.h"
#include <algorithm>

namespace msm {

Message NIStringParser::parse(const std::string& niString, const std::string& sender) {
    Message msg;
    std::string str = niString;
    
    auto trim = [](std::string& s) {
        size_t start = s.find_first_not_of(" \t");
        size_t end = s.find_last_not_of(" \t");
        if (start == std::string::npos) { s = ""; return; }
        s = s.substr(start, end - start + 1);
    };
    trim(str);
    
    if (str.empty()) {
        return msg;
    }
    
    size_t cmdEnd = str.find(">>");
    if (cmdEnd == std::string::npos) {
        cmdEnd = str.find("-@");
        if (cmdEnd == std::string::npos) {
            cmdEnd = str.find("->");
            if (cmdEnd == std::string::npos) {
                cmdEnd = str.size();
            }
        }
    }
    
    msg.cmd = str.substr(0, cmdEnd);
    trim(msg.cmd);
    
    std::string argsStr;
    std::string modeAndTarget;
    
    if (cmdEnd < str.size()) {
        std::string rest = str.substr(cmdEnd + 2);
        trim(rest);
        
        size_t arrowPos = rest.rfind("->");
        size_t syncPos = rest.rfind("-@");
        
        if (syncPos != std::string::npos && (arrowPos == std::string::npos || syncPos > arrowPos)) {
            argsStr = rest.substr(0, syncPos);
            modeAndTarget = rest.substr(syncPos);
        } else if (arrowPos != std::string::npos) {
            argsStr = rest.substr(0, arrowPos);
            modeAndTarget = rest.substr(arrowPos);
        } else {
            argsStr = rest;
        }
    }
    
    trim(argsStr);
    trim(modeAndTarget);
    
    if (!argsStr.empty()) {
        parseArgs(argsStr, msg.data);
    }
    
    std::string target;
    bool awaitResponse = false;
    std::string mode = "call";
    
    if (modeAndTarget.find("-@") != std::string::npos) {
        target = modeAndTarget.substr(2);
        trim(target);
        awaitResponse = true;
    } else if (modeAndTarget.find("->|") != std::string::npos) {
        target = modeAndTarget.substr(3);
        trim(target);
        awaitResponse = false;
    } else if (modeAndTarget.find("-><broadcast>") != std::string::npos) {
        target = "<broadcast>";
        awaitResponse = false;
        mode = "broadcast";
    } else if (modeAndTarget.find("-><register>") != std::string::npos) {
        target = "<register>";
        awaitResponse = false;
        mode = "register";
    } else if (modeAndTarget.find("-><unregister>") != std::string::npos) {
        target = "<unregister>";
        awaitResponse = false;
        mode = "unregister";
    } else if (modeAndTarget.find("->") != std::string::npos) {
        target = modeAndTarget.substr(2);
        trim(target);
        awaitResponse = true;
    }
    
    msg.setSender(sender);
    msg.setTarget(target);
    msg.setAwaitResponse(awaitResponse);
    msg.setMode(mode);
    msg.setCorrelationId(Message::generateCorrelationId());
    
    return msg;
}

void NIStringParser::parseArgs(const std::string& argsStr, Json& data) {
    std::stringstream ss(argsStr);
    std::string token;
    
    while (std::getline(ss, token, ',')) {
        size_t eqPos = token.find('=');
        if (eqPos != std::string::npos) {
            std::string key = token.substr(0, eqPos);
            std::string value = token.substr(eqPos + 1);
            
            auto trimStr = [](std::string& s) {
                size_t start = s.find_first_not_of(" \t");
                size_t end = s.find_last_not_of(" \t");
                if (start == std::string::npos) { s = ""; return; }
                s = s.substr(start, end - start + 1);
            };
            trimStr(key);
            trimStr(value);
            
            data.set(key, value);
        }
    }
}

std::string NIStringParser::toNIString(const Message& msg) {
    return msg.toNIString();
}

} // namespace msm