// Copyright (c) 2016 Alexander Gallego. All rights reserved.
//
#include "rpc/smf_gen/cpp_generator.h"
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <map>
#include <sstream>
#include "rpc/smf_gen/smf_file.h"
#include "rpc/smf_gen/smf_printer.h"

DEFINE_string(services_namespace, "", "puts the service into a namespace");
DEFINE_string(search_path, "", "prefix to any smf generator include");
DEFINE_bool(use_system_headers, true, "use #include<>, vs #include\"\"");

namespace smf_gen {
namespace {

template <class T> std::string as_string(T x) {
  std::ostringstream out;
  out << x;
  return out.str();
}

std::string file_name_identifier(const std::string &filename) {
  std::string result;
  for (unsigned i = 0; i < filename.size(); i++) {
    char c = filename[i];
    if (isalnum(c)) {
      result.push_back(toupper(c));
    } else if (c == '_') {
      result.push_back(c);
    } else {
      static char hex[] = "0123456789abcdef";
      result.push_back('_');
      result.push_back(hex[(c >> 4) & 0xf]);
      result.push_back(hex[c & 0xf]);
    }
  }
  return result;
}
}  // namespace

template <class T, size_t N> T *array_end(T (&array)[N]) { return array + N; }

void print_includes(smf_printer *                   printer,
                    const std::vector<std::string> &headers) {
  VLOG(1) << "print_includes";
  std::map<std::string, std::string> vars;
  vars["l"] = FLAGS_use_system_headers ? '<' : '"';
  vars["r"] = FLAGS_use_system_headers ? '>' : '"';
  if (!FLAGS_search_path.empty()) {
    vars["l"] += FLAGS_search_path;
    if (FLAGS_search_path.back() != '/') {
      vars["l"] += '/';
    }
  }
  for (auto i = headers.begin(); i != headers.end(); i++) {
    vars["h"] = *i;
    printer->print(vars, "#include $l$$h$$r$\n");
  }
}

std::string get_header_prologue(smf_file *file) {
  VLOG(1) << "get_header_prologue";
  std::string output;
  {
    // Scope the output stream so it closes and finalizes output to the string.
    auto printer = file->create_printer(&output);
    std::map<std::string, std::string> vars;

    // FIXME - weird ._generated.h
    vars["filename"] = file->filename();
    vars["filename_identifier"] =
      file_name_identifier(file->filename_without_path());
    vars["filename_base"]      = file->filename_without_ext();
    vars["message_header_ext"] = file->message_header_ext();

    printer->print("// Generated by the smf_gen.\n");
    printer->print("// Any local changes WILL BE LOST.\n");
    printer->print(vars, "// source: $filename$\n");
    printer->print("#pragma once\n");
    printer->print(vars, "#ifndef SMF_$filename_identifier$_INCLUDED\n");
    printer->print(vars, "#define SMF_$filename_identifier$_INCLUDED\n");
    printer->print("\n\n// hack: to use seastar's string type\n");
    printer->print("#include <core/sstring.hh>\n\n");
    printer->print(vars, "#include \"$filename_base$$message_header_ext$\"\n\n");
  }
  return output;
}

std::string get_header_includes(smf_file *file) {
  VLOG(1) << "get_header_includes";
  std::string output;
  {
    // Scope the output stream so it closes and finalizes output to the string.
    auto printer = file->create_printer(&output);
    std::map<std::string, std::string> vars;

    static const char *headers_strs[] = {
      "experimental/optional", "rpc/rpc_service.h", "rpc/rpc_client.h",
      "rpc/rpc_recv_typed_context.h", "platform/log.h"};

    std::vector<std::string> headers(headers_strs, array_end(headers_strs));
    print_includes(printer.get(), headers);
    printer->print("\n");

    if (!file->package().empty()) {
      std::vector<std::string> parts = file->package_parts();

      for (auto part = parts.begin(); part != parts.end(); part++) {
        vars["part"] = *part;
        printer->print(vars, "namespace $part$ {\n");
      }
      printer->print("\n");
    }
  }
  return output;
}


void print_header_service_index(smf_printer *      printer,
                                const smf_service *service) {
  VLOG(1) << "print_header_service_index for service: " << service->name();


  printer->print("virtual std::vector<smf::rpc_service_method_handle> "
                 "methods() override final {\n");
  printer->indent();
  printer->print("std::vector<smf::rpc_service_method_handle> handles;\n");

  for (int i = 0; i < service->method_count(); ++i) {
    auto method = service->method(i);
    std::map<std::string, std::string> vars;
    vars["MethodName"] = method->name();
    vars["InType"]     = method->input_type_name();
    vars["OutType"]    = method->output_type_name();
    vars["MethodId"]   = std::to_string(method->method_id());
    printer->print("handles.emplace_back(\n");
    printer->indent();
    printer->print(vars, "\"$MethodName$\", $MethodId$,\n");
    printer->print(
      "[this](smf::rpc_recv_context c) -> future<smf::rpc_envelope> {\n");
    printer->indent();
    printer->print(vars, "using t = smf::rpc_recv_typed_context<$InType$>;\n");
    printer->print(vars, "return $MethodName$(t(std::move(c)));\n");
    printer->outdent();
    printer->outdent();
    printer->print("});\n");
  }
  printer->print("return handles;\n");
  printer->outdent();
  printer->print("}\n");
}

void print_header_service_method(smf_printer *     printer,
                                 const smf_method *method) {
  VLOG(1) << "print_header_service_method: " << method->name();

  std::map<std::string, std::string> vars;
  vars["MethodName"] = method->name();
  vars["MethodId"]   = method->method_id();
  vars["InType"]     = method->input_type_name();
  vars["OutType"]    = method->output_type_name();
  printer->print("virtual future<smf::rpc_envelope>\n");
  printer->print(
    vars, "$MethodName$(smf::rpc_recv_typed_context<$InType$> &&rec) {\n");
  printer->indent();
  printer->print(vars, "// Output type: $OutType$\n");
  printer->print("smf::rpc_envelope e(nullptr);\n");
  printer->print(
    "// Helpful for clients to set the status.\n"
    "// Typically follows HTTP style. Not imposed by smf whatsoever.\n");
  printer->print("e.set_status(501); // Not implemented\n");
  printer->print(
    "return make_ready_future<smf::rpc_envelope>(std::move(e));\n");
  printer->outdent();
  printer->print("}\n");
}

void print_header_service(smf_printer *printer, const smf_service *service) {
  VLOG(1) << "print_header_service: " << service->name();
  std::map<std::string, std::string> vars{};
  vars["Service"]   = service->name();
  vars["ServiceID"] = std::to_string(service->service_id());

  printer->print(vars, "class $Service$: public smf::rpc_service {\n");
  printer->print(" public:\n");
  printer->indent();

  // print the overrides for smf
  printer->print("virtual const char *service_name() const override final {\n");
  printer->indent();
  printer->print(vars, "return \"$Service$\";\n");
  printer->outdent();
  printer->print("}\n");

  printer->print("virtual uint32_t service_id() const override final {\n");
  printer->indent();
  printer->print(vars, "return $ServiceID$;\n");
  printer->outdent();
  printer->print("}\n");

  print_header_service_index(printer, service);

  for (int i = 0; i < service->method_count(); ++i) {
    print_header_service_method(printer, service->method(i).get());
  }

  printer->outdent();
  printer->print(vars, "}; // end of service: $Service$\n");
}

bool is_camel_case(const std::string &s) {
  return std::find_if(s.begin(), s.end(), ::isupper) != s.end();
}
// copy on purpose
std::string proper_postfix_token(std::string s, std::string postfix) {
  CHECK(!s.empty() || !postfix.empty()) << "Can't compute postfix token";

  if (is_camel_case(s)) {
    s[0]       = toupper(s[0]);
    postfix[0] = toupper(postfix[0]);
    return s + postfix;
  }
  auto lowerstr = [](std::string &str) -> std::string {
    for (auto i = 0u; i < str.length(); ++i) {
      str[i] = tolower(str[i]);
    }
    return str;
  };
  return lowerstr(s) + std::string("_") + lowerstr(postfix);
}


void print_header_client_method(smf_printer *     printer,
                                const smf_method *method) {
  std::map<std::string, std::string> vars;
  vars["MethodName"]  = method->name();
  vars["MethodID"]    = std::to_string(method->method_id());
  vars["ServiceID"]   = std::to_string(method->service_id());
  vars["ServiceName"] = method->service_name();
  vars["InType"]      = method->input_type_name();
  vars["OutType"]     = method->output_type_name();

  printer->print(vars, "/// RequestID: $ServiceID$ ^ $MethodID$\n");
  printer->print(vars,
                 "/// ServiceID: $ServiceID$ == crc32(\"$ServiceName$\")\n");
  printer->print(vars,
                 "/// MethodID:  $MethodID$ == crc32(\"$MethodName$\")\n");
  printer->print(vars, "future<smf::rpc_recv_typed_context<$OutType$>>\n");
  printer->print(vars, "$MethodName$(smf::rpc_envelope e) {\n");
  printer->indent();
  printer->print(vars, "e.set_request_id($ServiceID$, $MethodID$);\n");
  printer->print(vars, "return send<$OutType$>(std::move(e),false);\n");
  printer->outdent();
  printer->print("}\n");
}

void print_safe_header_client_method(smf_printer *     printer,
                                     const smf_method *method) {
  std::map<std::string, std::string> vars;
  vars["MethodName"]       = method->name();
  vars["SafeMethodPrefix"] = std::islower(method->name()[0]) ? "safe_" : "Safe";
  vars["MethodID"]         = std::to_string(method->method_id());
  vars["ServiceID"]        = std::to_string(method->service_id());
  vars["ServiceName"]      = method->service_name();
  vars["InType"]           = method->input_type_name();
  vars["OutType"]          = method->output_type_name();

  printer->print(vars, "future<smf::rpc_recv_typed_context<$OutType$>>\n");
  printer->print(vars,
                 "$SafeMethodPrefix$$MethodName$(smf::rpc_envelope e) {\n");
  printer->indent();
  printer->print(
    "return limit_.wait(1).then([this, e=std::move(e)]() mutable {\n");
  printer->indent();
  printer->print(vars,
                 "return this->$MethodName$(std::move(e)).finally([this](){\n");
  printer->indent();
  printer->print("limit_.signal(1);\n");
  printer->outdent();
  printer->print("});\n");
  printer->outdent();
  printer->print("});\n");
  printer->outdent();
  printer->print("}\n");
}
void print_header_client(smf_printer *printer, const smf_service *service) {
  // print the client rpc code
  VLOG(1) << "print_header_client for service: " << service->name();
  std::map<std::string, std::string> vars{};
  vars["ClientName"] = proper_postfix_token(service->name(), "client");
  vars["ServiceID"]  = std::to_string(service->service_id());

  printer->print(vars, "class $ClientName$: public smf::rpc_client {\n"
                       " public:\n");
  printer->indent();

  // print ctor
  printer->print(vars, "$ClientName$(ipv4_addr "
                       "server_addr)\n:smf::rpc_client(std::move(server_addr))"
                       " {}\n");

  printer->outdent();
  printer->print("\n");
  printer->indent();

  for (int i = 0; i < service->method_count(); ++i) {
    print_header_client_method(printer, service->method(i).get());
    print_safe_header_client_method(printer, service->method(i).get());
  }

  printer->outdent();
  printer->print(vars, "}; // end of rpc client: $ClientName$\n");
}

std::string get_header_services(smf_file *file) {
  VLOG(1) << "get_header_services";

  std::string output;
  {
    // Scope the output stream so it closes and finalizes output to the string.
    auto printer = file->create_printer(&output);
    std::map<std::string, std::string> vars;
    // Package string is empty or ends with a dot. It is used to fully qualify
    // method names.
    vars["Package"] = file->package();
    if (!file->package().empty()) {
      vars["Package"].append(".");
    }

    if (!FLAGS_services_namespace.empty()) {
      vars["services_namespace"] = FLAGS_services_namespace;
      printer->print(vars, "\nnamespace $services_namespace$ {\n\n");
    }

    for (int i = 0; i < file->service_count(); ++i) {
      print_header_service(printer.get(), file->service(i).get());
      printer->print("\n");
    }

    for (int i = 0; i < file->service_count(); ++i) {
      print_header_client(printer.get(), file->service(i).get());
      printer->print("\n");
    }

    if (!FLAGS_services_namespace.empty()) {
      printer->print(vars, "}  // namespace $services_namespace$\n\n");
    }
  }
  return output;
}

std::string get_header_epilogue(smf_file *file) {
  VLOG(1) << "get_header_epilogue";

  std::string output;
  {
    // Scope the output stream so it closes and finalizes output to the string.
    auto printer = file->create_printer(&output);
    std::map<std::string, std::string> vars;

    vars["filename"] = file->filename();
    vars["filename_identifier"] =
      file_name_identifier(file->filename_without_path());

    if (!file->package().empty()) {
      std::vector<std::string> parts = file->package_parts();

      for (auto part = parts.rbegin(); part != parts.rend(); part++) {
        vars["part"] = *part;
        printer->print(vars, "}  // namespace $part$\n");
      }
      printer->print(vars, "\n");
    }

    printer->print(vars, "\n");
    printer->print(vars, "#endif  // SMF_$filename_identifier$_INCLUDED\n");
  }
  return output;
}


}  // namespace smf_gen
