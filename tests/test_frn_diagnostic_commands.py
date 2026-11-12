"""Exercise the production private-message allowlist, secret parser and logic case handling."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
source=(root/'src/svxlink/modules/frn/ModuleFrn.cpp').read_text()
helpers=source[source.index('static inline std::string trim_copy('):source.index('static inline std::string runCmdHelpLine(')]
parser=source[source.index('bool ModuleFrn::parseAndAuthorizeCmd('):source.index('void ModuleFrn::sendCmdReplyChunked(')]
chunker=source[source.index('void ModuleFrn::sendCmdReplyChunked('):source.index('void ModuleFrn::onTextMessageReceived(')]
unit=r'''
#include <string>
#include <vector>
#include <algorithm>
#include <sstream>
#include <cctype>
#include <cassert>
#include <iostream>
static const size_t FRN_TM_MAX_CHARS=240;
class Peer { public: std::vector<std::string> replies;
 void sendTextMessage(const std::string&,const std::string& text) { replies.push_back(text); }
};
class ModuleFrn {
public:
 std::string run_cmd_secret;
 Peer* qso=nullptr;
 bool parseAndAuthorizeCmd(const std::string&, const std::string&, std::string&, std::string&);
 void sendCmdReplyChunked(const std::string&, const std::string&);
};
'''+helpers+parser+chunker+r'''
int main() {
 ModuleFrn module; std::string cmd, error;
 for(const auto* text : {"get diagnostics", "get diagnostics node", "get diagnostics logic", "get diagnostics all"}) {
   cmd.clear(); error.clear();
   assert(module.parseAndAuthorizeCmd("peer",std::string("RunCmd: ")+text,cmd,error));
   assert(error.empty() && cmd==text);
 }
 assert(module.parseAndAuthorizeCmd("peer","rUnCmD: GET DIAGNOSTICS LOGIC LocalRepeater2",cmd,error));
 assert(cmd=="get diagnostics logic LocalRepeater2" && error.empty());
 module.run_cmd_secret="CaseSensitive";
 cmd.clear(); error.clear();
 assert(module.parseAndAuthorizeCmd("peer","RunCmd:CaseSensitive: GET DIAGNOSTICS LOGIC MySimplex",cmd,error));
 assert(error.empty() && cmd=="get diagnostics logic MySimplex");
 error.clear();
 assert(module.parseAndAuthorizeCmd("peer","RunCmd:casesensitive: get diagnostics",cmd,error));
 assert(error.find("auth")!=std::string::npos);
 module.run_cmd_secret.clear();
 for(const auto* text : {"get stats", "get diagnostics logic MySimplex;reboot", "set usb power 2 off"}) {
   cmd.clear(); error.clear();
   assert(module.parseAndAuthorizeCmd("peer",std::string("RunCmd: ")+text,cmd,error));
   assert(!error.empty() && cmd.empty());
 }
 assert(!module.parseAndAuthorizeCmd("peer","ordinary FRN text",cmd,error));
 Peer peer; module.qso=&peer;
 std::string large;
 for(unsigned i=0;i<10000;++i) large+="field"+std::to_string(i)+"=value ";
 large+="END";
 module.sendCmdReplyChunked("peer",large);
 assert(peer.replies.size()>99);
 std::string rebuilt;
 for(const auto& reply: peer.replies) {
   assert(reply.size()<=FRN_TM_MAX_CHARS);
   const auto prefix=reply.find("] "); assert(prefix!=std::string::npos);
   if(!rebuilt.empty()) rebuilt+=' ';
   rebuilt+=reply.substr(prefix+2);
 }
 assert(rebuilt==large);
 std::cout << "FRN scoped commands, named logic case, authentication and rejected legacy/injected commands passed\n";
}
'''
with tempfile.TemporaryDirectory(prefix='frn-diagnostic-parser-') as directory:
    path=Path(directory)
    (path/'test.cpp').write_text(unit)
    subprocess.run(['g++','-std=c++11','-Wall','-Wextra','-Werror',str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
