#include <Common/parseAddress.h>
#include <Common/Exception.h>
#include <IO/ReadHelpers.h>
#include <base/find_symbols.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}

std::pair<std::string, UInt16> parseAddress(const std::string & str, UInt16 default_port)
{
    if (str.empty())
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Empty address passed to function parseAddress");

    const char * begin = str.data();
    const char * end = begin + str.size();
    const char * port = end; // NOLINT

    if (begin[0] == '[')
    {
        const char * closing_square_bracket = find_first_symbols<']'>(begin + 1, end);
        if (closing_square_bracket >= end)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Illegal address passed to function parseAddress: "
                            "the address begins with opening square bracket, but no closing square bracket found");

        port = closing_square_bracket + 1;
    }
    else
        port = find_first_symbols<':'>(begin, end);

    if (port != end)
    {
        if (*port != ':')
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "Illegal port prefix passed to function parseAddress: {}", port);

        ++port;

        UInt16 port_number = 0;
        ReadBufferFromMemory port_buf(port, end - port);
        if (!tryReadText(port_number, port_buf) || !port_buf.eof())
        {
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "Illegal port passed to function parseAddress: {}", port);
        }
        return { std::string(begin, port - 1), port_number };
    }
    if (default_port)
    {
        return {str, default_port};
    }
    throw Exception(
        ErrorCodes::BAD_ARGUMENTS,
        "The address passed to function parseAddress doesn't contain port number and no 'default_port' was passed");
}

std::pair<std::string, UInt16> parseAddressFromURL(const std::string & url, UInt16 default_port)
{
    std::string_view authority = url;

    /// Strip a "scheme://" prefix if present.
    if (auto scheme_end = authority.find("://"); scheme_end != std::string_view::npos)
        authority.remove_prefix(scheme_end + 3);

    /// The authority ends at the first '/', so drop any "/path" that follows.
    if (auto slash = authority.find('/'); slash != std::string_view::npos)
        authority = authority.substr(0, slash);

    /// Strip "userinfo@". Split on the last '@' so a password that itself contains '@' is handled.
    if (auto at = authority.rfind('@'); at != std::string_view::npos)
        authority.remove_prefix(at + 1);

    /// An empty authority means the client library connects to localhost by default.
    if (authority.empty())
        return {"localhost", default_port};

    return parseAddress(std::string(authority), default_port);
}

}
