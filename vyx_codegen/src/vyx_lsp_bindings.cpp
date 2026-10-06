#include "vyx_codegen.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" VYX_API void* vyx_lsp_bind_owner(void* node);
extern "C" VYX_API int32_t vyx_lsp_bind_decl_needed(void* node);
extern "C" int64_t vyx_rt_process_private_bytes();

// The frontend supplies source spans BEFORE rewriting names and exports actual
// Sema targets. Only compact, value-owned facts survive a per-file service frame.
// Packed node offsets below are the AST ABI in bootstrap/core/ast.vyx (64 bit).
namespace {
static_assert(sizeof(void*)==8, "LSP binding export requires the 64-bit frontend AST ABI");
template<class T> T slot(void* p, size_t off) {
    T value{}; if (p) std::memcpy(&value, static_cast<char*>(p) + off, sizeof(T));
    return value;
}
void* ptr(void* p, size_t off) { return slot<void*>(p, off); }
std::string text(void* p, size_t off, size_t len) {
    auto s = slot<const char*>(p, off); auto n = slot<int64_t>(p, len);
    return s && n > 0 ? std::string(s, size_t(n)) : std::string();
}
std::string path_key(std::string s) {
    if(s.empty()) return {};
    if (s.compare(0, 7, "file://") == 0) {
        s.erase(0, 7); std::string decoded;
        const auto hex = [](char c) { return c >= '0' && c <= '9' ? c-'0' :
            c >= 'a' && c <= 'f' ? c-'a'+10 : c >= 'A' && c <= 'F' ? c-'A'+10 : -1; };
        for (size_t i=0; i<s.size(); ++i) {
            if (s[i]=='%' && i+2<s.size() && hex(s[i+1])>=0 && hex(s[i+2])>=0) {
                decoded += char(hex(s[i+1])*16 + hex(s[i+2])); i+=2;
            } else decoded += s[i];
        } s = std::move(decoded);
        if (s.size()>3 && s[0]=='/' && s[2]==':') s.erase(0,1);
    }
    for (auto& c:s) if(c=='\\') c='/';
    std::error_code ec;
    auto path = std::filesystem::absolute(std::filesystem::u8path(s),ec);
    s = (ec ? std::filesystem::u8path(s) : path).lexically_normal().generic_u8string();
#ifdef _WIN32
    for(auto& c:s) if(c>='A' && c<='Z') c += 'a'-'A';
#endif
    return s;
}
std::string uri(std::string s) {
    std::string result="file://"; if(s.empty() || s[0]!='/') result+='/' ;
    const char* hex="0123456789ABCDEF";
    for(unsigned char c:s) {
        if(c=='/' || c==':' || c=='-' || c=='_' || c=='.' || c=='~' ||
           (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9')) result+=char(c);
        else { result+='%'; result+=hex[c>>4]; result+=hex[c&15]; }
    } return result;
}
std::string quote(const std::string& s) {
    std::string out="\""; const char* hex="0123456789abcdef";
    for(unsigned char c:s) {
        if(c=='"' || c=='\\') { out+='\\'; out+=char(c); }
        else if(c<32) { out+="\\u00"; out+=hex[c>>4]; out+=hex[c&15]; }
        else out+=char(c);
    } return out+'"';
}
struct Span {
    std::string file, word; int line=0, col=0, length=0;
    std::string key() const { return file+"#"+std::to_string(line)+":"+std::to_string(col); }
};
struct Fact { Span span; std::string binding, underlying, definition_file; bool declaration=false, editable=true, local=false; };
struct File {
    std::string path, source, module, identifiers, symbols, coverage; int version=-1;
    bool writable=false, open=false, metadata_dirty=true; uint64_t epoch=0;
    std::vector<Fact> facts; int errors=0;
};
struct Node { void* node=nullptr; int role=0, original_kind=0; Span span; void* owner=nullptr; void* target=nullptr;
    int end_line=0,end_col=0; std::string module, scope; };
struct Index {
    std::map<std::string,File> files;
    std::map<std::string,int32_t> standard_roots;
    int32_t standard_provider=-1;
    std::string root, registry, output, error, parse_file, analyzing, origin, new_name, binding, word;
    std::vector<std::string> parse_stack, candidates, siblings;
    std::unordered_map<void*,Node> nodes;
    std::vector<void*> types;
    std::unordered_map<void*,void*> owners;
    std::unordered_map<void*,bool> needed;
    std::unordered_map<std::string,Span> decl_locations;
    uint64_t epoch=1; size_t next=0, sibling_next=0, type_next=0;
    int mode=0, line=0, character=0; bool declarations=false, versioned=false, active=false;
    int parse_errors=0;
    std::vector<Fact> selected;
    std::map<std::string,File> backup;
    std::map<std::string,std::vector<Fact>> proposed;
    uint64_t saved_epoch=0;
};
thread_local Index* g_lsp_index=nullptr;
bool trace_facts() { auto value=std::getenv("VYX_LSP_BIND_TRACE"); return value && std::strcmp(value,"phases")!=0; }
bool identifier(unsigned char c) { return c=='_' || (c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9'); }
// This lexical inventory is ONLY a conservative candidate filter. It never
// binds identifiers. Binding and rename safety come from the compiler's Sema.
void inventory(File& f) {
    f.module.clear();
    std::set<std::string> names; std::vector<std::string> tokens;
    auto& s=f.source; size_t i=0;
    while(i<s.size()) {
        if(i+1<s.size() && s[i]=='/' && s[i+1]=='/') { i=s.find('\n',i); if(i==std::string::npos) break; continue; }
        if(i+1<s.size() && s[i]=='/' && s[i+1]=='*') {
            i+=2; int depth=1; while(i<s.size() && depth) {
                if(i+1<s.size() && s[i]=='/' && s[i+1]=='*') { ++depth; i+=2; }
                else if(i+1<s.size() && s[i]=='*' && s[i+1]=='/') { --depth; i+=2; }
                else ++i;
            } continue;
        }
        if(s[i]=='"' || s[i]=='\'') {
            const size_t start=i;
            const char end=s[i++]; while(i<s.size()) { if(s[i]=='\\') i+=std::min<size_t>(2,s.size()-i); else if(s[i++]==end) break; }
            if(s.substr(start,i-start).find("${")!=std::string::npos) {
                for(size_t j=start;j<i;) {
                    if(identifier((unsigned char)s[j])) { const auto a=j++; while(j<i && identifier((unsigned char)s[j])) ++j; names.insert(s.substr(a,j-a)); }
                    else ++j;
                }
            } continue;
        }
        if(identifier((unsigned char)s[i])) {
            size_t start=i++; while(i<s.size() && identifier((unsigned char)s[i])) ++i;
            auto word=s.substr(start,i-start); names.insert(word); tokens.push_back(std::move(word));
        } else { if(s[i]=='.' || s[i]==';' || s[i]=='{') tokens.emplace_back(1,s[i]); ++i; }
    }
    f.identifiers="|"; for(auto& n:names) f.identifiers+=n+"|";
    f.module.clear();
    for(size_t j=0;j+1<tokens.size();++j) if(tokens[j]=="module") {
        std::string m=tokens[++j]; while(j+2<tokens.size() && tokens[j+1]==".") { m+='.'; m+=tokens[j+2]; j+=2; }
        if(j+1<tokens.size() && tokens[j+1]==";") f.module=m;
        break;
    }
}
int utf16(const std::string& s, size_t start, size_t end) {
    int col=0; for(size_t i=start;i<end && i<s.size();) {
        unsigned char c=s[i]; size_t n=c<128?1:c<224?2:c<240?3:4;
        col+=n==4?2:1; i+=std::min(n,end-i);
    } return col;
}
size_t offset(const File& f, int line, int bytecol) {
    size_t i=0; while(line-->0) { i=f.source.find('\n',i); if(i==std::string::npos) return f.source.size(); ++i; }
    return std::min(f.source.size(),i+size_t(std::max(0,bytecol)));
}
std::string range(const Span& s) {
    auto it=g_lsp_index->files.find(s.file); if(it==g_lsp_index->files.end()) return "null";
    auto& f=it->second; size_t a=offset(f,s.line,0), b=offset(f,s.line,s.col);
    int col=utf16(f.source,a,b), end=utf16(f.source,a,b+s.length);
    return "{\"start\":{\"line\":"+std::to_string(s.line)+",\"character\":"+std::to_string(col)+"},\"end\":{\"line\":"+std::to_string(s.line)+",\"character\":"+std::to_string(end)+"}}";
}
std::string location(const Span& s) { return "{\"uri\":"+quote(uri(g_lsp_index->files.at(s.file).path))+",\"range\":"+range(s)+"}"; }
std::string decl_location(void* d) {
    auto meta=text(d,88,96); auto a=meta.find("|source-path:");
    std::string file;
    if(a!=std::string::npos) { a+=13; file=path_key(meta.substr(a,meta.find('|',a)-a)); }
    return file+"#"+std::to_string(slot<int32_t>(d,136))+":"+std::to_string(slot<int32_t>(d,140));
}
Span* declaration(void* d) {
    auto it=g_lsp_index->nodes.find(d);
    if(it!=g_lsp_index->nodes.end() && (it->second.role==1 || it->second.role==4 || it->second.role==8)) return &it->second.span;
    if(trace_facts()) std::fprintf(stderr,"unmapped declaration %s loc=%s meta=%s\n",text(d,16,24).c_str(),decl_location(d).c_str(),text(d,88,96).c_str());
    auto pos=g_lsp_index->decl_locations.find(decl_location(d));
    return pos==g_lsp_index->decl_locations.end()?nullptr:&pos->second;
}
std::string declaration_binding(void* d) {
    auto source=declaration(d); if(!source) return {};
    int kind=slot<int32_t>(d,0);
    if(kind!=1 && kind!=2 && kind!=5) return source->key();
    auto file=g_lsp_index->files.find(source->file); if(file==g_lsp_index->files.end()) return {};
    std::string owner;
    auto parent=g_lsp_index->owners.find(d);
    if(parent!=g_lsp_index->owners.end()) if(auto span=declaration(parent->second)) owner=span->key();
    // Overloads are one source name. Grouping uses the exact module and owner
    // declaration identity, never a suffix match across unrelated types.
    auto n=g_lsp_index->nodes.find(d);
    auto module=n!=g_lsp_index->nodes.end() && !n->second.scope.empty()?n->second.scope:file->second.module;
    return "F\x1f"+module+"\x1f"+owner+"\x1f"+source->word;
}
bool declaration_editable(void* d) {
    if(!d) return true;
    if(slot<int32_t>(d,0)==2) return false; // External ABI names require producer changes.
    auto meta=text(d,88,96);
    if(meta.find("|foreign_dci")!=std::string::npos || meta.find("|foreign_cpp")!=std::string::npos) return false;
    auto parent=g_lsp_index->owners.find(d);
    return parent==g_lsp_index->owners.end() || !parent->second || parent->second==d || declaration_editable(parent->second);
}
void walk_expr(void*,void*,std::unordered_set<void*>&);
void walk_type(void*,void*,std::unordered_set<void*>&);
void walk_decl(void*,void*,std::unordered_set<void*>&);
void walk_stmt(void* s,void* owner,std::unordered_set<void*>& seen) {
    for(;s;s=ptr(s,72)) {
        if(!seen.insert(s).second) return;
        g_lsp_index->owners.emplace(s,owner);
        if(slot<int32_t>(s,0)==10 || slot<int32_t>(s,0)==13) walk_stmt(ptr(s,32),owner,seen);
        else walk_expr(ptr(s,32),owner,seen);
        walk_expr(ptr(s,40),owner,seen);
        walk_type(ptr(s,24),owner,seen);
        walk_stmt(ptr(s,48),owner,seen); walk_stmt(ptr(s,56),owner,seen); walk_stmt(ptr(s,64),owner,seen);
    }
}
void walk_expr(void* e,void* owner,std::unordered_set<void*>& seen) {
    if(!e || !seen.insert(e).second) return; g_lsp_index->owners.emplace(e,owner);
    const int kind=slot<int32_t>(e,0);
    if(kind==15) {
        walk_stmt(ptr(e,48),owner,seen); walk_type(ptr(e,56),owner,seen);
        walk_decl(ptr(e,64),owner,seen); walk_stmt(ptr(e,80),owner,seen); return;
    }
    walk_expr(ptr(e,48),owner,seen);
    if(kind==27) {
        walk_stmt(ptr(e,56),owner,seen);
        auto branch=ptr(e,64);
        if(branch && slot<int32_t>(branch,0)==27) walk_expr(branch,owner,seen);
        else walk_stmt(branch,owner,seen);
        return;
    }
    walk_expr(ptr(e,56),owner,seen);
    if(kind==19) walk_expr(ptr(e,64),owner,seen); else walk_stmt(ptr(e,64),owner,seen);
    if(kind==28 || kind==29) walk_type(ptr(e,80),owner,seen);
    if(kind==17) walk_expr(ptr(e,80),owner,seen);
    if(kind==22) walk_type(ptr(e,72),owner,seen);
}
void walk_type(void* t,void* owner,std::unordered_set<void*>& seen) {
    if(!t || !seen.insert(t).second) return; g_lsp_index->owners.emplace(t,owner);
    const int kind=slot<int32_t>(t,0);
    if(kind==8 || kind==11 || kind==12) walk_type(ptr(t,8),owner,seen);
    if(kind==13) { walk_type(ptr(t,8),owner,seen); walk_expr(ptr(t,16),owner,seen); }
    if(kind==14) { auto box=ptr(t,8); for(int64_t i=0;i<slot<int64_t>(t,16);++i) walk_type(ptr(box,size_t(i)*8),owner,seen); }
    if(kind==23 || kind==25) { walk_type(ptr(t,8),owner,seen); walk_type(ptr(t,16),owner,seen); }
    // Source nominal nodes are recorded individually by the parser, including
    // nested function/tuple/array element types. No text-based type matching.
}
void walk_decl(void* d,void* parent,std::unordered_set<void*>& seen) {
    for(;d;d=ptr(d,64)) {
        if(!seen.insert(d).second) return;
        g_lsp_index->owners.emplace(d,parent);
        walk_type(ptr(d,32),d,seen); walk_decl(ptr(d,40),d,seen);
        walk_stmt(ptr(d,48),d,seen); walk_decl(ptr(d,56),d,seen);
        if(auto s=declaration(d)) g_lsp_index->decl_locations[decl_location(d)]=*s;
    }
}
const Fact* at(const File& f,int line,int character) {
    for(auto& fact:f.facts) {
        if(fact.span.line!=line) continue;
        auto a=offset(f,line,0), b=offset(f,line,fact.span.col);
        int start=utf16(f.source,a,b),end=utf16(f.source,a,b+fact.span.length);
        if(character>=start && character<end && !fact.binding.empty()) return &fact;
    } return nullptr;
}
bool registry_file(const File& f) {
    const auto path=path_key(f.path);
    for(auto& root:g_lsp_index->standard_roots)
        if(path.compare(0,root.first.size(),root.first)==0 &&
           (path.size()==root.first.size() || path[root.first.size()]=='/'))
            return root.second==g_lsp_index->standard_provider;
    return true;
}
void rebuild_registry() {
    g_lsp_index->registry.clear(); for(auto& entry:g_lsp_index->files) {
        auto& f=entry.second; if(!f.module.empty() && registry_file(f)) g_lsp_index->registry+=f.module+"\t"+f.path+"\t"+f.symbols+"\n";
    }
}
}

extern "C" VYX_API void vyx_lsp_bind_create(const char* root) {
    delete g_lsp_index; g_lsp_index=new Index; g_lsp_index->root=path_key(root?root:"");
}
extern "C" VYX_API int32_t vyx_lsp_bind_enabled() { return g_lsp_index && g_lsp_index->active; }
extern "C" VYX_API void vyx_lsp_bind_destroy() { delete g_lsp_index; g_lsp_index=nullptr; }
// One complete standard-library provider enters Sema. Keep other copies in the
// source g_lsp_index for navigation/read-only checks, but never merge their declarations
// into the same unit. A provider's many files (including same-module files) remain.
extern "C" VYX_API void vyx_lsp_bind_package_root(const char* root,int32_t priority) {
    if(!g_lsp_index || !root || !*root) return;
    auto key=path_key(root);
    auto inserted=g_lsp_index->standard_roots.emplace(key,priority);
    if(!inserted.second) inserted.first->second=std::min(inserted.first->second,priority);
    if(g_lsp_index->standard_provider<0 || priority<g_lsp_index->standard_provider) g_lsp_index->standard_provider=priority;
    ++g_lsp_index->epoch; rebuild_registry();
}
extern "C" VYX_API void vyx_lsp_bind_file(const char* path,const char* source,int64_t length,int32_t version,int32_t writable) {
    if(!g_lsp_index || !path || length<0) return; auto key=path_key(path); auto& f=g_lsp_index->files[key];
    std::string next(source?source:"",size_t(length));
    if(f.path.empty()) f.writable=writable!=0;
    if(f.path.empty() || f.source!=next) { ++g_lsp_index->epoch; f.source=std::move(next); f.metadata_dirty=true; inventory(f); }
    // Registry paths and result URIs must also be absolute, not just map keys.
    f.path=key;
    // Opening a pre-indexed SDK source must not turn it into a user declaration.
    f.version=version; f.open=version>=0;
}
extern "C" VYX_API void vyx_lsp_bind_close(const char* path) {
    if(!g_lsp_index) return; auto it=g_lsp_index->files.find(path_key(path?path:"")); if(it==g_lsp_index->files.end()) return;
    std::ifstream file(std::filesystem::u8path(it->second.path),std::ios::binary);
    if(!file) { g_lsp_index->files.erase(it); ++g_lsp_index->epoch; return; }
    std::string source((std::istreambuf_iterator<char>(file)),{});
    auto original=it->second.path; auto writable=it->second.writable;
    vyx_lsp_bind_file(original.c_str(),source.data(),source.size(),-1,writable);
}
extern "C" VYX_API void vyx_lsp_bind_file_symbols(const char* path,void* unit) {
    if(!g_lsp_index) return; auto it=g_lsp_index->files.find(path_key(path?path:"")); if(it==g_lsp_index->files.end()) return;
    std::set<std::string> names;
    for(auto d=ptr(unit,0);d;d=ptr(d,64)) {
        const int kind=slot<int32_t>(d,0);
        if(kind==6 || kind==7 || kind==4 || kind==5) continue;
        auto name=text(d,16,24); if(!name.empty()) names.insert(name);
    }
    std::string symbols="|"; for(auto& name:names) symbols+=name+"|";
    if(it->second.symbols!=symbols) { it->second.symbols=std::move(symbols); ++g_lsp_index->epoch; }
    it->second.metadata_dirty=false;
    rebuild_registry();
}
extern "C" VYX_API const char* vyx_lsp_bind_metadata_next() {
    if(!g_lsp_index) return nullptr;
    for(auto& entry:g_lsp_index->files) if(entry.second.metadata_dirty) {
        entry.second.metadata_dirty=false; return entry.second.path.c_str();
    } return nullptr;
}
extern "C" VYX_API const char* vyx_lsp_bind_source(const char* path,int64_t* length) {
    if(!g_lsp_index) return nullptr; auto it=g_lsp_index->files.find(path_key(path?path:""));
    if(it==g_lsp_index->files.end()) return nullptr; *length=it->second.source.size(); return it->second.source.data();
}
extern "C" VYX_API void vyx_lsp_bind_parse_push(const char* path) {
    if(!g_lsp_index || !g_lsp_index->active) return; g_lsp_index->parse_stack.push_back(g_lsp_index->parse_file); g_lsp_index->parse_file=path_key(path?path:"");
}
extern "C" VYX_API void vyx_lsp_bind_parse_pop() {
    if(!g_lsp_index || !g_lsp_index->active || g_lsp_index->parse_stack.empty()) return;
    g_lsp_index->parse_file=std::move(g_lsp_index->parse_stack.back()); g_lsp_index->parse_stack.pop_back();
}
extern "C" VYX_API void vyx_lsp_bind_span(void* node,int32_t role,int32_t line,int32_t col,const char* word,int64_t length) {
    if(!g_lsp_index || !g_lsp_index->active || !node || line<1 || col<1 || length<=0) return;
    Node n; n.node=node; n.role=role; n.span={g_lsp_index->parse_file,std::string(word,size_t(length)),line-1,col-1,int(length)};
    n.original_kind=slot<int32_t>(node,0);
    auto f=g_lsp_index->files.find(n.span.file); if(f==g_lsp_index->files.end()) { if(trace_facts()) std::fprintf(stderr,"unmapped source %s name=%s\n",n.span.file.c_str(),n.span.word.c_str()); return; }
    size_t a=offset(f->second,n.span.line,n.span.col);
    if(f->second.source.compare(a,length,n.span.word)!=0) { if(trace_facts()) std::fprintf(stderr,"span mismatch %s %d:%d word=%s actual=%s\n",n.span.file.c_str(),line,col,n.span.word.c_str(),f->second.source.substr(a,length).c_str()); return; }
    g_lsp_index->nodes[node]=std::move(n);
}
extern "C" VYX_API void vyx_lsp_bind_copy(void* from,void* to) {
    if(!g_lsp_index || !g_lsp_index->active) return; auto it=g_lsp_index->nodes.find(from);
    if(it!=g_lsp_index->nodes.end()) { auto n=it->second; n.node=to; g_lsp_index->nodes[to]=std::move(n); }
}
extern "C" VYX_API void vyx_lsp_bind_target(void* node,void* target) {
    if(!g_lsp_index || !g_lsp_index->active) return; auto it=g_lsp_index->nodes.find(node);
    if(it!=g_lsp_index->nodes.end()) it->second.target=target;
}
extern "C" VYX_API void vyx_lsp_bind_module(void* node,const char* name,int64_t length) {
    if(!g_lsp_index || !g_lsp_index->active) return; auto it=g_lsp_index->nodes.find(node);
    if(it!=g_lsp_index->nodes.end()) it->second.module="M:"+std::string(name,size_t(length));
}
extern "C" VYX_API void vyx_lsp_bind_decl_scope(void* node,const char* scope,int64_t length) {
    if(!g_lsp_index || !g_lsp_index->active) return; auto it=g_lsp_index->nodes.find(node);
    if(it!=g_lsp_index->nodes.end()) it->second.scope=std::string(scope,size_t(length));
}
extern "C" VYX_API void vyx_lsp_bind_parse_errors(int32_t count) {
    if(g_lsp_index && g_lsp_index->active) g_lsp_index->parse_errors+=count;
}
extern "C" VYX_API void vyx_lsp_bind_failed() {
    if(!g_lsp_index || !g_lsp_index->active) return;
    auto& f=g_lsp_index->files.at(g_lsp_index->analyzing);
    f.facts.clear(); f.errors=std::max(1,g_lsp_index->parse_errors); f.epoch=g_lsp_index->epoch;
    f.coverage=g_lsp_index->word+"\x1e"+g_lsp_index->new_name;
    g_lsp_index->nodes.clear(); g_lsp_index->owners.clear(); g_lsp_index->decl_locations.clear();
    g_lsp_index->types.clear(); g_lsp_index->parse_stack.clear(); g_lsp_index->active=false;
}
extern "C" VYX_API void vyx_lsp_bind_decl_end(void* node,int32_t line,int32_t col) {
    if(!g_lsp_index || !g_lsp_index->active) return; auto it=g_lsp_index->nodes.find(node);
    if(it!=g_lsp_index->nodes.end()) { it->second.end_line=line-1; it->second.end_col=col-1; }
}
extern "C" VYX_API void vyx_lsp_bind_prepare(void* unit) {
    if(!g_lsp_index || !g_lsp_index->active) return;
    if(std::getenv("VYX_LSP_BIND_TRACE")) std::fprintf(stderr,"prepare %s nodes=%zu private_mib=%lld\n",g_lsp_index->analyzing.c_str(),g_lsp_index->nodes.size(),static_cast<long long>(vyx_rt_process_private_bytes()/1048576));
    std::unordered_set<void*> seen; walk_decl(ptr(unit,0),nullptr,seen);
    g_lsp_index->needed.clear();
    g_lsp_index->types.clear(); g_lsp_index->type_next=0;
    for(auto& entry:g_lsp_index->nodes) {
        auto& n=entry.second;
        if(!g_lsp_index->owners.count(n.node)) {
            Node* best=nullptr;
            for(auto& candidate:g_lsp_index->nodes) {
                auto& d=candidate.second;
                if(d.role!=1 || d.span.file!=n.span.file || (!d.end_line && !d.end_col)) continue;
                if(std::tie(n.span.line,n.span.col)<std::tie(d.span.line,d.span.col) ||
                   std::tie(n.span.line,n.span.col)>=std::tie(d.end_line,d.end_col)) continue;
                if(!best || std::tie(d.span.line,d.span.col)>std::tie(best->span.line,best->span.col)) best=&d;
            }
            if(best) g_lsp_index->owners[n.node]=best->node;
        }
        // Imported bodies supply declaration identities only. Resolving their
        // untyped expressions here would type the dependency graph a second
        // time, including bodies unrelated to this file's query.
        auto owner=vyx_lsp_bind_owner(n.node);
        const bool callable=owner && (slot<int32_t>(owner,0)==1 || slot<int32_t>(owner,0)==5);
        const bool needed=!callable || vyx_lsp_bind_decl_needed(owner)!=0;
        if(n.span.file==g_lsp_index->analyzing && needed &&
           (n.role==3 || n.role==6 || n.role==13 || n.role==9 ||
           (n.role==2 && !ptr(n.node,96) && !n.target))) g_lsp_index->types.push_back(entry.first);
    }
    if(std::getenv("VYX_LSP_BIND_TRACE")) std::fprintf(stderr,"prepared %s types=%zu owners=%zu private_mib=%lld\n",g_lsp_index->analyzing.c_str(),g_lsp_index->types.size(),g_lsp_index->owners.size(),static_cast<long long>(vyx_rt_process_private_bytes()/1048576));
}
extern "C" VYX_API void* vyx_lsp_bind_next_type() {
    if(!g_lsp_index || !g_lsp_index->active || g_lsp_index->type_next>=g_lsp_index->types.size()) return nullptr;
    if(std::getenv("VYX_LSP_BIND_TRACE") && g_lsp_index->type_next%1000==0) std::fprintf(stderr,"types %s at=%zu private_mib=%lld\n",g_lsp_index->analyzing.c_str(),g_lsp_index->type_next,static_cast<long long>(vyx_rt_process_private_bytes()/1048576));
    return g_lsp_index->types[g_lsp_index->type_next++];
}
extern "C" VYX_API void* vyx_lsp_bind_owner(void* node) {
    if(!g_lsp_index || !g_lsp_index->active) return nullptr; auto it=g_lsp_index->owners.find(node);
    auto owner=it==g_lsp_index->owners.end()?nullptr:it->second;
    while(owner) {
        const int kind=slot<int32_t>(owner,0);
        if(kind==1 || kind==3 || kind==5 || kind==8 || kind==11) return owner;
        auto parent=g_lsp_index->owners.find(owner); if(parent==g_lsp_index->owners.end() || parent->second==owner) return nullptr;
        owner=parent->second;
    } return nullptr;
}
extern "C" VYX_API int32_t vyx_lsp_bind_generic_target(void* node,void* owner,const char* name,int64_t length) {
    if(!g_lsp_index || !g_lsp_index->active || !owner) return 0;
    for(auto& entry:g_lsp_index->nodes) {
        auto& n=entry.second;
        if(n.role==8 && n.span.word==std::string(name,size_t(length)) && vyx_lsp_bind_owner(n.node)==owner) {
            vyx_lsp_bind_target(node,n.node); return 1;
        }
    } return 0;
}
extern "C" VYX_API int32_t vyx_lsp_bind_role(void* node) {
    if(!g_lsp_index || !g_lsp_index->active) return 0; auto it=g_lsp_index->nodes.find(node); return it==g_lsp_index->nodes.end()?0:it->second.role;
}
extern "C" VYX_API const char* vyx_lsp_bind_name(void* node) {
    if(!g_lsp_index || !g_lsp_index->active) return "";
    auto it=g_lsp_index->nodes.find(node); return it==g_lsp_index->nodes.end()?"":it->second.span.word.c_str();
}
extern "C" VYX_API const char* vyx_lsp_bind_root() { return g_lsp_index && g_lsp_index->active?g_lsp_index->files.at(g_lsp_index->analyzing).path.c_str():nullptr; }
extern "C" VYX_API int32_t vyx_lsp_bind_decl_is_root(void* node) {
    if(!g_lsp_index || !g_lsp_index->active) return 0;
    auto span=declaration(node); return span && span->file==g_lsp_index->analyzing;
}
extern "C" VYX_API int32_t vyx_lsp_bind_decl_needed(void* node) {
    if(!g_lsp_index || !g_lsp_index->active) return 1;
    auto cached=g_lsp_index->needed.find(node); if(cached!=g_lsp_index->needed.end()) return cached->second;
    auto it=g_lsp_index->nodes.find(node); if(it==g_lsp_index->nodes.end() || it->second.span.file!=g_lsp_index->analyzing) return 0;
    auto& n=it->second; auto& f=g_lsp_index->files.at(g_lsp_index->analyzing);
    // Selecting a local, parameter or generic must type its entire lexical scope.
    if(g_lsp_index->analyzing==g_lsp_index->origin && g_lsp_index->line>=n.span.line && g_lsp_index->line<=n.end_line) {
        const auto start=utf16(f.source,offset(f,n.span.line,0),offset(f,n.span.line,n.span.col));
        if(g_lsp_index->line>n.span.line || g_lsp_index->character>=start) return g_lsp_index->needed[node]=true;
    }
    const size_t a=offset(f,n.span.line,n.span.col), b=offset(f,n.end_line,n.end_col);
    File part; part.source=f.source.substr(a,b>=a?b-a:0); inventory(part);
    bool needed=!g_lsp_index->word.empty() && part.identifiers.find("|"+g_lsp_index->word+"|")!=std::string::npos;
    if(!g_lsp_index->new_name.empty()) needed=needed || part.identifiers.find("|"+g_lsp_index->new_name+"|")!=std::string::npos;
    // Aliases are candidate spellings only. Sema still resolves their targets.
    for(auto& entry:g_lsp_index->nodes) if(entry.second.role==5 && entry.second.span.file==g_lsp_index->analyzing)
        needed=needed || part.identifiers.find("|"+entry.second.span.word+"|")!=std::string::npos;
    g_lsp_index->needed[node]=needed; return needed;
}
extern "C" VYX_API const char* vyx_lsp_bind_sibling() {
    if(!g_lsp_index || !g_lsp_index->active || g_lsp_index->sibling_next>=g_lsp_index->siblings.size()) return nullptr;
    return g_lsp_index->files.at(g_lsp_index->siblings[g_lsp_index->sibling_next++]).path.c_str();
}
extern "C" VYX_API void vyx_lsp_bind_finish(int32_t errors) {
    if(!g_lsp_index || !g_lsp_index->active) return;
    auto& f=g_lsp_index->files.at(g_lsp_index->analyzing); f.facts.clear(); f.errors=errors+g_lsp_index->parse_errors;
    std::unordered_map<void*,std::string> locals;
    std::unordered_map<std::string,std::pair<std::string,std::string>> aliases;
    for(auto& entry:g_lsp_index->nodes) {
        auto& n=entry.second;
        if(n.role==4 || (n.role==11 && ptr(n.node,32)) || (n.role==1 && slot<int32_t>(n.node,120)==1)) {
            void* sym=ptr(n.node,n.role==4?88:128); if(sym) locals[sym]=n.span.key();
        }
        if(n.role==5) aliases[n.span.file+"|"+n.span.word]={n.span.key(),""};
    }
    for(auto& entry:g_lsp_index->nodes) {
        auto& n=entry.second; if(n.span.file!=g_lsp_index->analyzing) continue;
        Fact fact; fact.span=n.span; fact.definition_file=n.span.file;
        if(n.role==1) fact.editable=declaration_editable(n.node);
        void* target=n.target;
        if(!n.module.empty()) { fact.binding=n.module; fact.declaration=n.role==9; }
        else if(n.role==1 || n.role==4 || n.role==5 || n.role==8 || (n.role==11 && ptr(n.node,32))) {
            fact.binding=n.span.key(); fact.declaration=true;
            fact.local=n.role==4 || n.role==5 || n.role==8 || n.role==11 || slot<int32_t>(n.node,120)==1;
            if(n.role==1 && !fact.local) fact.binding=declaration_binding(n.node);
        } else if(n.role==2) {
            int semantic=slot<int32_t>(n.node,88); if(ptr(n.node,96)) target=ptr(n.node,96);
            if(semantic==1 || semantic==10) {
                auto local=locals.find(target); if(local!=locals.end()) { fact.binding=local->second; fact.local=true; }
            } else if(target) { if(auto s=declaration(target)) { fact.binding=declaration_binding(target); fact.definition_file=s->file; }
                else if(semantic==5 && n.original_kind==5) {
                    if(auto s=declaration(ptr(n.node,104))) if(n.span.word==s->word) {
                        fact.binding=s->key(); fact.definition_file=s->file;
                    }
                }
            }
        } else if(n.role==11) {
            auto local=locals.find(ptr(n.node,88)); if(local!=locals.end()) fact.binding=local->second;
            fact.local=true;
        } else if(n.role==12) {
            if(auto s=declaration(g_lsp_index->owners[n.node])) fact.binding=s->key();
        } else if(target) { if(auto s=declaration(target)) { fact.binding=declaration_binding(target); fact.definition_file=s->file; } }
        auto target_node=g_lsp_index->nodes.find(target);
        if(target && !fact.local) fact.editable=declaration_editable(target);
        if(target_node!=g_lsp_index->nodes.end() && target_node->second.role==8) fact.local=true;
        if(n.role!=5 && n.role!=1 && n.role!=4 && !fact.local && !fact.binding.empty()) {
            auto alias=aliases.find(n.span.file+"|"+n.span.word);
            if(alias!=aliases.end() && n.role!=6 && (n.role==3 || n.role==13 ||
                (n.role==2 && n.original_kind==5 && slot<int32_t>(n.node,88)!=1)) && fact.binding!=alias->second.first) {
                fact.underlying=fact.binding; fact.binding=alias->second.first;
                fact.local=true; fact.definition_file=n.span.file;
                fact.editable=true;
                alias->second.second=fact.underlying;
            }
        }
        if(trace_facts()) std::fprintf(stderr,"binding %s %d:%d %s role=%d sem=%d target=%p key=%s\n",n.span.file.c_str(),n.span.line,n.span.col,n.span.word.c_str(),n.role,n.role==2?slot<int32_t>(n.node,88):0,target,fact.binding.c_str());
        if(!fact.binding.empty()) f.facts.push_back(std::move(fact));
    }
    std::sort(f.facts.begin(),f.facts.end(),[](const Fact& a,const Fact& b) { return std::tie(a.span.line,a.span.col,a.binding)<std::tie(b.span.line,b.span.col,b.binding); });
    f.facts.erase(std::unique(f.facts.begin(),f.facts.end(),[](const Fact& a,const Fact& b) { return a.span.key()==b.span.key() && a.binding==b.binding; }),f.facts.end());
    f.epoch=g_lsp_index->epoch;
    f.coverage=g_lsp_index->word+"\x1e"+g_lsp_index->new_name;
    if(std::getenv("VYX_LSP_BIND_TRACE")) std::fprintf(stderr,"finish %s facts=%zu errors=%d\n",g_lsp_index->analyzing.c_str(),f.facts.size(),f.errors);
    g_lsp_index->nodes.clear(); g_lsp_index->owners.clear(); g_lsp_index->decl_locations.clear(); g_lsp_index->types.clear(); g_lsp_index->parse_stack.clear();
}
extern "C" VYX_API void vyx_lsp_bind_query(const char* path,int32_t line,int32_t character,int32_t mode,const char* new_name,int32_t declarations,int32_t versioned) {
    if(!g_lsp_index) return;
    g_lsp_index->origin=path_key(path?path:""); g_lsp_index->line=line; g_lsp_index->character=character; g_lsp_index->mode=mode;
    g_lsp_index->word.clear();
    auto source=g_lsp_index->files.find(g_lsp_index->origin);
    if(source!=g_lsp_index->files.end()) {
        auto& f=source->second; size_t begin=offset(f,line,0), at=begin;
        while(at<f.source.size() && f.source[at]!='\n' && utf16(f.source,begin,at)<character) {
            const unsigned char c=f.source[at];
            at+=std::min<size_t>(c<128?1:c<224?2:c<240?3:4,f.source.size()-at);
        }
        if(at<f.source.size() && identifier((unsigned char)f.source[at])) {
            size_t a=at,b=at; while(a>begin && identifier((unsigned char)f.source[a-1])) --a;
            while(b<f.source.size() && identifier((unsigned char)f.source[b])) ++b;
            g_lsp_index->word=f.source.substr(a,b-a);
        }
    }
    g_lsp_index->new_name=new_name?new_name:""; g_lsp_index->declarations=declarations!=0; g_lsp_index->versioned=versioned!=0;
    g_lsp_index->selected.clear(); g_lsp_index->error.clear(); g_lsp_index->binding.clear(); g_lsp_index->candidates={g_lsp_index->origin}; g_lsp_index->next=0;
    // Disk files may change without a watcher. Open buffers remain authoritative.
    std::vector<std::string> disk; for(auto& entry:g_lsp_index->files) if(!entry.second.open) disk.push_back(entry.first);
    for(auto& key:disk) { auto it=g_lsp_index->files.find(key); if(it!=g_lsp_index->files.end()) vyx_lsp_bind_close(it->second.path.c_str()); }
    // Watch notifications are optional in LSP. Discover new user files even
    // when a client does not install watchers, retaining the same exclusions.
    if(!g_lsp_index->root.empty()) {
        std::error_code ec;
        std::filesystem::recursive_directory_iterator iter(std::filesystem::u8path(g_lsp_index->root),
            std::filesystem::directory_options::skip_permission_denied,ec), end;
        const std::set<std::string> excluded={".git",".cache","node_modules","target","out","dist","build",
            ".idea",".vs",".vscode",".runs",".worktrees",".gradle",".intellijPlatform",".kotlin",
            "cmake-build-debug","cmake-build-release","deprecated_seeds","seed"};
        while(iter!=end && !ec) {
            auto entry=*iter; auto name=entry.path().filename().u8string();
            if(entry.is_directory(ec)) {
                if(iter.depth()>=12 || excluded.count(name) || name.compare(0,5,"seed_")==0 || entry.is_symlink(ec)) iter.disable_recursion_pending();
            } else if(entry.path().extension()==".vyx") {
                auto filename=entry.path().generic_u8string(), key=path_key(filename);
                if(!g_lsp_index->files.count(key)) {
                    std::ifstream file(entry.path(),std::ios::binary);
                    if(file) { std::string source((std::istreambuf_iterator<char>(file)),{});
                        vyx_lsp_bind_file(filename.c_str(),source.data(),source.size(),-1,1); }
                }
            }
            iter.increment(ec);
        }
    }
    rebuild_registry();
}
extern "C" VYX_API const char* vyx_lsp_bind_registry() { return g_lsp_index?g_lsp_index->registry.c_str():""; }
extern "C" VYX_API const char* vyx_lsp_bind_next() {
    if(!g_lsp_index) return nullptr;
    if(g_lsp_index->mode!=3 && g_lsp_index->next==1 && g_lsp_index->binding.empty()) {
        auto it=g_lsp_index->files.find(g_lsp_index->origin); if(it==g_lsp_index->files.end()) return nullptr;
        auto origin=at(it->second,g_lsp_index->line,g_lsp_index->character); if(!origin) return nullptr;
        g_lsp_index->binding=origin->binding;
        std::string word=origin->span.word;
        for(auto& entry:g_lsp_index->files) for(auto& fact:entry.second.facts)
            if(fact.declaration && fact.binding==g_lsp_index->binding) word=fact.span.word;
        g_lsp_index->word=word;
        if(g_lsp_index->mode==0 || g_lsp_index->mode==1) {
            auto bound_file=origin->definition_file;
            if(bound_file!=g_lsp_index->origin && g_lsp_index->files.count(bound_file)) g_lsp_index->candidates.push_back(bound_file);
        }
        if(!origin->local && g_lsp_index->mode!=1 && g_lsp_index->mode!=4) for(auto& entry:g_lsp_index->files) {
            auto& f=entry.second;
            if(entry.first!=g_lsp_index->origin &&
                std::find(g_lsp_index->candidates.begin(),g_lsp_index->candidates.end(),entry.first)==g_lsp_index->candidates.end() &&
                f.writable && registry_file(f) &&
                (f.identifiers.find("|"+word+"|")!=std::string::npos ||
                 (!g_lsp_index->new_name.empty() && f.identifiers.find("|"+g_lsp_index->new_name+"|")!=std::string::npos))) g_lsp_index->candidates.push_back(entry.first);
        }
    }
    while(g_lsp_index->next<g_lsp_index->candidates.size()) {
        auto key=g_lsp_index->candidates[g_lsp_index->next++]; auto it=g_lsp_index->files.find(key); if(it==g_lsp_index->files.end()) continue;
        g_lsp_index->analyzing=key;
        if(std::getenv("VYX_LSP_BIND_TRACE")) std::fprintf(stderr,"analyzing %s epoch=%llu cached=%llu candidates=%zu\n",key.c_str(),(unsigned long long)g_lsp_index->epoch,(unsigned long long)it->second.epoch,g_lsp_index->candidates.size());
        if(it->second.epoch==g_lsp_index->epoch && it->second.coverage==g_lsp_index->word+"\x1e"+g_lsp_index->new_name) {
            if(g_lsp_index->next==1) return vyx_lsp_bind_next(); continue;
        }
        g_lsp_index->active=true; g_lsp_index->parse_errors=0; g_lsp_index->nodes.clear(); g_lsp_index->owners.clear(); g_lsp_index->decl_locations.clear(); g_lsp_index->parse_file=key;
        g_lsp_index->siblings.clear(); g_lsp_index->sibling_next=0;
        if(!it->second.module.empty()) for(auto& entry:g_lsp_index->files)
            if(entry.first!=key && entry.second.module==it->second.module && registry_file(entry.second)) g_lsp_index->siblings.push_back(entry.first);
        return it->second.path.c_str();
    }
    g_lsp_index->active=false; return nullptr;
}
extern "C" VYX_API const char* vyx_lsp_bind_result() {
    if(!g_lsp_index) return "null";
    auto it=g_lsp_index->files.find(g_lsp_index->origin); auto origin=it==g_lsp_index->files.end()?nullptr:at(it->second,g_lsp_index->line,g_lsp_index->character);
    g_lsp_index->output="null"; if(!origin) { if(g_lsp_index->mode==0 || g_lsp_index->mode==4) g_lsp_index->output="[]"; return g_lsp_index->output.c_str(); }
    if(g_lsp_index->mode==1) {
        bool writable=false; for(auto& entry:g_lsp_index->files) for(auto& fact:entry.second.facts)
            if(fact.declaration && fact.binding==origin->binding && fact.editable && entry.second.writable) writable=true;
        if(writable) g_lsp_index->output=range(origin->span);
        return g_lsp_index->output.c_str();
    }
    if(g_lsp_index->mode==2) {
        auto definition=g_lsp_index->files.find(origin->definition_file);
        if(definition==g_lsp_index->files.end() || !definition->second.writable) {
            g_lsp_index->error="The declaration belongs to a read-only SDK or dependency."; return "null";
        }
    }
    if(g_lsp_index->binding.empty()) g_lsp_index->binding=origin->binding;
    std::vector<Fact> facts; std::set<std::string> seen;
    for(auto& key:g_lsp_index->candidates) {
        auto file=g_lsp_index->files.find(key); if(file==g_lsp_index->files.end()) continue;
        for(auto& fact:file->second.facts) {
            if(fact.binding!=g_lsp_index->binding && !(g_lsp_index->mode==0 && fact.underlying==g_lsp_index->binding)) continue;
            if((g_lsp_index->mode==0 && !g_lsp_index->declarations && fact.declaration) || !seen.insert(fact.span.key()).second) continue;
            facts.push_back(fact);
        }
    }
    if(g_lsp_index->mode==0 || g_lsp_index->mode==4) {
        g_lsp_index->output="["; for(auto& fact:facts) {
            if(g_lsp_index->mode==4 && fact.span.file!=g_lsp_index->origin) continue;
            if(g_lsp_index->output.size()>1) g_lsp_index->output+=',';
            g_lsp_index->output+=g_lsp_index->mode==4?"{\"range\":"+range(fact.span)+",\"kind\":1}":location(fact.span);
        } g_lsp_index->output+=']'; return g_lsp_index->output.c_str();
    }
    std::map<std::string,std::vector<Fact>> edits;
    for(auto& fact:facts) {
        auto& f=g_lsp_index->files.at(fact.span.file);
        if(!f.writable || !fact.editable) { g_lsp_index->error="Rename cannot modify a read-only dependency or an external ABI declaration."; break; }
        if(f.errors) { g_lsp_index->error="Rename requires successful semantic analysis of every affected source file."; break; }
        edits[fact.span.file].push_back(fact);
    }
    if(edits.empty() && g_lsp_index->error.empty()) g_lsp_index->error="No writable source declaration was resolved.";
    if(!g_lsp_index->error.empty()) return "null";
    g_lsp_index->selected=facts;
    g_lsp_index->output=g_lsp_index->versioned?"{\"documentChanges\":[":"{\"changes\":{";
    bool first=true;
    for(auto& entry:edits) {
        auto& f=g_lsp_index->files.at(entry.first); if(!first) g_lsp_index->output+=','; first=false;
        if(g_lsp_index->versioned) g_lsp_index->output+="{\"textDocument\":{\"uri\":"+quote(uri(f.path))+",\"version\":"+(f.open?std::to_string(f.version):"null")+"},\"edits\":[";
        else g_lsp_index->output+=quote(uri(f.path))+": [";
        bool first_edit=true;
        for(auto& fact:entry.second) { if(!first_edit) g_lsp_index->output+=','; first_edit=false;
            g_lsp_index->output+="{\"range\":"+range(fact.span)+",\"newText\":"+quote(g_lsp_index->new_name)+"}"; }
        g_lsp_index->output+=g_lsp_index->versioned?"]}":"]";
    }
    g_lsp_index->output+=g_lsp_index->versioned?"]}":"}}"; return g_lsp_index->output.c_str();
}
extern "C" VYX_API const char* vyx_lsp_bind_error() { return g_lsp_index?g_lsp_index->error.c_str():""; }

namespace {
Span moved_span(Span span) {
    auto edits=g_lsp_index->proposed.find(span.file); if(edits==g_lsp_index->proposed.end()) return span;
    int delta=0;
    for(auto& edit:edits->second) if(edit.span.line==span.line && edit.span.col<span.col)
        delta+=int(g_lsp_index->new_name.size())-edit.span.length;
    span.col+=delta; return span;
}
std::string moved_binding(const std::string& binding) {
    if(binding.compare(0,2,"F\x1f")==0) {
        const auto a=binding.find('\x1f',2), b=binding.find('\x1f',a+1);
        std::string module=binding.substr(2,a-2), owner=binding.substr(a+1,b-a-1), name=binding.substr(b+1);
        if(g_lsp_index->binding.compare(0,2,"M:")==0) {
            const auto old=g_lsp_index->binding.substr(2);
            if(module==old || module.compare(0,old.size()+1,old+".")==0) {
                const auto parent=old.rfind('.');
                module=(parent==std::string::npos?std::string():old.substr(0,parent+1))+g_lsp_index->new_name+module.substr(old.size());
            }
        }
        if(!owner.empty()) owner=moved_binding(owner);
        for(auto& entry:g_lsp_index->proposed) for(auto& fact:entry.second)
            if(fact.declaration && fact.binding==binding) { name=g_lsp_index->new_name; break; }
        return "F\x1f"+module+"\x1f"+owner+"\x1f"+name;
    }
    if(binding.compare(0,2,"M:")==0 && g_lsp_index->binding.compare(0,2,"M:")==0 &&
        (binding==g_lsp_index->binding || binding.compare(0,g_lsp_index->binding.size()+1,g_lsp_index->binding+".")==0)) {
        auto parent=g_lsp_index->binding.rfind('.');
        auto prefix=parent==std::string::npos?std::string("M:"):g_lsp_index->binding.substr(0,parent+1);
        return prefix+g_lsp_index->new_name+binding.substr(g_lsp_index->binding.size());
    }
    if(binding.compare(0,2,"M:")==0) return binding;
    for(auto& entry:g_lsp_index->backup) for(auto& fact:entry.second.facts)
        if(fact.declaration && fact.binding==binding) return moved_span(fact.span).key();
    return binding;
}
}
// Validate proposed edits with the same compiler and a temporary source overlay.
// Rebinding every previously resolved occurrence detects both duplicate names
// and capture of unrelated references; merely counting diagnostics is insufficient.
extern "C" VYX_API int32_t vyx_lsp_bind_validate_begin() {
    if(!g_lsp_index || g_lsp_index->mode!=2 || !g_lsp_index->error.empty() || g_lsp_index->selected.empty()) return 0;
    g_lsp_index->backup.clear(); g_lsp_index->proposed.clear(); g_lsp_index->saved_epoch=g_lsp_index->epoch;
    for(auto& key:g_lsp_index->candidates) {
        auto it=g_lsp_index->files.find(key); if(it!=g_lsp_index->files.end()) g_lsp_index->backup[key]=it->second;
    }
    for(auto& fact:g_lsp_index->selected) g_lsp_index->proposed[fact.span.file].push_back(fact);
    // Keep declaration positions from imported targets too, so expected target
    // identities are remapped when preceding edits move their source columns.
    for(auto& entry:g_lsp_index->files) if(!g_lsp_index->backup.count(entry.first) && !entry.second.facts.empty())
        g_lsp_index->backup[entry.first]=entry.second;
    for(auto& entry:g_lsp_index->proposed) {
        auto& f=g_lsp_index->files.at(entry.first);
        auto edits=entry.second;
        std::sort(edits.begin(),edits.end(),[](const Fact& a,const Fact& b) {
            return std::tie(a.span.line,a.span.col)>std::tie(b.span.line,b.span.col);
        });
        for(auto& edit:edits) {
            const auto start=offset(f,edit.span.line,edit.span.col);
            if(f.source.compare(start,edit.span.length,edit.span.word)!=0) {
                g_lsp_index->error="Source changed while preparing rename."; break;
            }
            f.source.replace(start,edit.span.length,g_lsp_index->new_name);
            if(edit.declaration) {
                const auto token="|"+edit.span.word+"|";
                auto found=f.symbols.find(token);
                if(found!=std::string::npos) f.symbols.replace(found,token.size(),"|"+g_lsp_index->new_name+"|");
            }
        }
        inventory(f);
    }
    ++g_lsp_index->epoch; rebuild_registry(); g_lsp_index->mode=3; g_lsp_index->next=0; return 1;
}
extern "C" VYX_API int32_t vyx_lsp_bind_validate_end() {
    if(!g_lsp_index || g_lsp_index->mode!=3) return 0;
    for(auto& key:g_lsp_index->candidates) {
        if(!g_lsp_index->error.empty()) break;
        auto old=g_lsp_index->backup.find(key), now=g_lsp_index->files.find(key);
        if(old==g_lsp_index->backup.end() || now==g_lsp_index->files.end()) continue;
        if(now->second.errors) { g_lsp_index->error="Rename introduces a semantic error or a conflicting declaration."; break; }
        for(auto& fact:old->second.facts) {
            auto expected=moved_span(fact.span); bool found=false;
            for(auto& next:now->second.facts) {
                if(next.span.key()!=expected.key()) continue;
                if(next.binding==moved_binding(fact.binding) &&
                    next.underlying==moved_binding(fact.underlying)) { found=true; break; }
            }
            if(!found) {
                g_lsp_index->error="Rename would change or lose an existing semantic binding at "+
                    uri(old->second.path)+":"+std::to_string(fact.span.line+1)+":"+std::to_string(fact.span.col+1)+".";
                break;
            }
        }
    }
    for(auto& entry:g_lsp_index->backup) g_lsp_index->files[entry.first]=std::move(entry.second);
    g_lsp_index->backup.clear(); g_lsp_index->proposed.clear(); g_lsp_index->epoch=g_lsp_index->saved_epoch;
    rebuild_registry(); g_lsp_index->mode=2; g_lsp_index->active=false;
    return g_lsp_index->error.empty();
}
