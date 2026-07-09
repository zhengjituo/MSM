#ifndef MSM_NI_STRING_PARSER_H
#define MSM_NI_STRING_PARSER_H

#include "Message.h"
#include <string>

namespace msm {

class NIStringParser {
public:
    static Message parse(const std::string& niString, const std::string& sender = "");
    static std::string toNIString(const Message& msg);
    
private:
    static void parseArgs(const std::string& argsStr, Json& data);
};

} // namespace msm

#endif // MSM_NI_STRING_PARSER_H