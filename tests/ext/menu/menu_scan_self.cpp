// The scanner the menu text tests stand on (menu_scan.h) tested on its own, each rule with a positive and a negative control:
// StripComments (character literals such as '"', digit separators, raw strings reported as unsupported, #if 0 blocks),
// FunctionBodies/FunctionBody/Reaches (brace matching that ignores braces in strings and character literals, dead
// functions are not reachable) and the ImGui stack balance counters (a missing End/Pop, a count argument, a return between
// a push and its pop, returns inside a lambda).
#include "menu_scan.h"

#include <cstdio>
#include <string>

namespace
{
int failures = 0;
void Check(bool ok, const char* what, int line)
{
    if (!ok)
    {
        std::printf("FAIL line %d: %s\n", line, what);
        ++failures;
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

using menuscan::Squash;

std::string Strip(const std::string& s)
{
    bool raw = false;
    const std::string out = menuscan::StripCommentsChecked(s, raw);
    return menuscan::StripIfZero(out);
}

bool IsRaw(const std::string& s)
{
    bool raw = false;
    menuscan::StripCommentsChecked(s, raw);
    return raw;
}

bool Balanced(const std::string& body, const char* open)
{
    for (const auto& p : menuscan::StackPairs())
        if (std::string(p.open) == open)
        {
            std::string why;
            return menuscan::StackBalanced(body, p, why);
        }
    return false;
}

bool ReturnInside(const std::string& body, const char* open)
{
    for (const auto& p : menuscan::StackPairs())
        if (std::string(p.open) == open)
        {
            size_t at = 0;
            return menuscan::ReturnInsideStack(body, p, at);
        }
    return false;
}

void StripCommentsRules()
{
    // a '"' character literal does not open a string: the comment after it is still removed, the real string is kept
    CHECK(Strip("char q = '\"'; // c\nconst char* s = \"a//b\"; /* x */ int y;") ==
          "char q = '\"'; \nconst char* s = \"a//b\";  int y;\n");
    // escaped quote and backslash literals
    CHECK(Strip("char a = '\\''; // c \"\nint b;") == "char a = '\\''; \nint b;\n");
    CHECK(Strip("char a = '\\\\'; // c\nint b;") == "char a = '\\\\'; \nint b;\n");
    // a brace in a character literal is kept as text (the brace matcher skips it later)
    CHECK(Strip("char o = '{'; // c").find("'{'") != std::string::npos);
    // a digit separator is not a character literal: the comment after it goes
    CHECK(Strip("int a = 1'000; // \"c\nint b;") == "int a = 1'000; \nint b;\n");
    // strings keep their //
    CHECK(Strip("const char* u = \"http://x\"; // gone") == "const char* u = \"http://x\"; \n");

    // raw strings are reported (the output must not be trusted), plain strings that merely end in R are not
    CHECK(IsRaw("auto s = R\"(a // b)\";"));
    CHECK(IsRaw("auto s = u8R\"x(a)x\";"));
    CHECK(IsRaw("auto s = LR\"(a)\";"));
    CHECK(!IsRaw("const char* s = \"abR\"; int x;"));
    CHECK(!IsRaw("int FOR = 1; const char* s = \"x\";"));

    // #if 0 blocks: removed; the #else branch is live again; nested #if inside the dead block is skipped as a whole
    CHECK(Strip("a();\n#if 0\ndead();\n#endif\nb();") == "a();\nb();\n");
    CHECK(Strip("#if 0\ndead();\n#else\nlive();\n#endif\nafter();") == "live();\nafter();\n");
    CHECK(Strip("#if 0\n#ifdef X\nd1();\n#endif\nd2();\n#endif\nafter();") == "after();\n");
    CHECK(Strip("#if 01\nlive();\n#endif").find("live();") != std::string::npos); // only the plain `#if 0`
    CHECK(Strip("#ifdef X\nlive();\n#endif").find("live();") != std::string::npos);
}

void FunctionBodyRules()
{
    const std::string code = Strip(
        "namespace ext\n{\nnamespace\n{\n"
        "struct Holder\n{\n    void Method() const { Inner(); }\n};\n"
        "void Inner() { const char* s = \"}{\"; char c = '}'; }\n"
        "void DrawCard(int x)\n{\n    if (x) { Inner(); }\n    auto f = [&](int y) { return y; };\n}\n"
        "void Dead() { Inner(); }\n"
        "void Lone() { }\n"
        "AutoRegister reg { { \"id\", \"tab\", DrawCard } };\n"
        "} // namespace\n} // namespace ext\n");
    const auto funcs = menuscan::FunctionBodies(code);
    std::string names;
    for (const auto& f : funcs)
        names += f.name + ",";
    CHECK(names == "Method,Inner,DrawCard,Dead,Lone,"); // the data brace of `reg` and the lambda are not functions of their own

    CHECK(menuscan::FunctionBody(code, "Inner").find("'}'") != std::string::npos); // braces in a string and a char did not end it
    CHECK(menuscan::FunctionBody(code, "Inner").back() == '}');
    CHECK(menuscan::FunctionBody(code, "DrawCard").find("return y") != std::string::npos);
    CHECK(menuscan::FunctionBody(code, "Nope").empty());
    CHECK(menuscan::FunctionBody(code + "void Lone() { }", "Lone").empty()); // two definitions: no answer

    // reachability: DrawCard calls Inner; Dead is called by nobody
    CHECK(menuscan::Reaches(code, "DrawCard", "Inner"));
    CHECK(menuscan::Reaches(code, "DrawCard", "DrawCard"));
    CHECK(!menuscan::Reaches(code, "DrawCard", "Dead"));
    CHECK(!menuscan::Reaches(code, "DrawCard", "Lone"));
    // the same file with the call renamed: Inner is no longer reachable from DrawCard
    std::string broken = code;
    broken.replace(broken.find("if (x) { Inner(); }"), 19, "if (x) { Inner2(); }");
    CHECK(!menuscan::Reaches(broken, "DrawCard", "Inner"));
    // through a helper: A -> B -> C
    const std::string chain = Strip("void C() {}\nvoid B() { C(); }\nvoid A() { B(); }\n");
    CHECK(menuscan::Reaches(chain, "A", "C"));
    CHECK(!menuscan::Reaches(chain, "B", "A"));
}

void StackRules()
{
    using menuscan::Squash;
    const std::string ok = "{ ImGui::BeginDisabled(!a); ImGui::Button(\"x\"); ImGui::EndDisabled(); }";
    CHECK(Balanced(ok, "ImGui::BeginDisabled("));
    CHECK(!Balanced("{ ImGui::BeginDisabled(!a); ImGui::Button(\"x\"); }", "ImGui::BeginDisabled(")); // no End
    CHECK(!Balanced("{ ImGui::EndDisabled(); }", "ImGui::BeginDisabled("));                               // extra End
    CHECK(Balanced("{ ImGui::PushItemWidth(100); ImGui::PopItemWidth(); }", "ImGui::PushItemWidth("));
    CHECK(!Balanced("{ ImGui::PushItemWidth(100); }", "ImGui::PushItemWidth("));
    CHECK(Balanced("{ ImGui::Indent(8.0f); ImGui::Unindent(8.0f); }", "ImGui::Indent("));
    CHECK(!Balanced("{ ImGui::Indent(8.0f); }", "ImGui::Indent("));
    CHECK(Balanced("{ ImGui::PushID(\"a\"); ImGui::PopID(); }", "ImGui::PushID("));
    // PopStyleColor takes a count
    CHECK(Balanced("{ ImGui::PushStyleColor(a, b); ImGui::PushStyleColor(c, d); ImGui::PopStyleColor(2); }",
                   "ImGui::PushStyleColor("));
    CHECK(!Balanced("{ ImGui::PushStyleColor(a, b); ImGui::PushStyleColor(c, d); ImGui::PopStyleColor(); }",
                    "ImGui::PushStyleColor("));
    CHECK(!Balanced("{ ImGui::PushStyleColor(a, b); ImGui::PopStyleColor(2); }", "ImGui::PushStyleColor("));
    CHECK(Balanced("{ ImGui::PushStyleColor(a, b); ImGui::PopStyleColor(); }", "ImGui::PushStyleColor("));

    // a return between the push and the pop
    CHECK(!ReturnInside(ok, "ImGui::BeginDisabled("));
    CHECK(ReturnInside("{ ImGui::BeginDisabled(x); if (y) return; ImGui::EndDisabled(); }", "ImGui::BeginDisabled("));
    CHECK(!ReturnInside("{ if (y) return; ImGui::BeginDisabled(x); ImGui::EndDisabled(); return; }", "ImGui::BeginDisabled("));
    CHECK(!ReturnInside("{ ImGui::BeginDisabled(x); auto f = [&](int v) { return v; }; ImGui::EndDisabled(); }",
                        "ImGui::BeginDisabled(")); // a lambda's return is not the function's
    CHECK(ReturnInside("{ ImGui::BeginDisabled(x); auto f = [&](int v) { return v; }; return; ImGui::EndDisabled(); }",
                       "ImGui::BeginDisabled(")); // ... but one after the lambda is
    CHECK(ReturnInside("{ ImGui::PushItemWidth(5); if (z) { return; } ImGui::PopItemWidth(); }", "ImGui::PushItemWidth("));
    // a close on a branch that leaves (continue/break/return) is that branch's own: the normal path still needs its close
    CHECK(Balanced("{ ImGui::PushID(i); if (a) { ImGui::PopID(); continue; } ImGui::PopID(); }", "ImGui::PushID("));
    CHECK(!Balanced("{ ImGui::PushID(i); if (a) { ImGui::PopID(); continue; } }", "ImGui::PushID("));
    CHECK(!Balanced("{ ImGui::PushID(i); if (a) { ImGui::PopID(); return; } }", "ImGui::PushID("));
    CHECK(!ReturnInside("{ ImGui::PushID(i); if (a) { ImGui::PopID(); continue; } ImGui::PopID(); }", "ImGui::PushID("));
    CHECK(!ReturnInside("{ ImGui::PushID(i); if (a) { ImGui::PopID(); return; } ImGui::PopID(); }", "ImGui::PushID("));
    CHECK(ReturnInside("{ ImGui::PushID(i); if (a) { ImGui::PopID(); continue; } if (b) return; ImGui::PopID(); }",
                       "ImGui::PushID(")); // the normal path is still open at the second return
    CHECK(!ReturnInside("{ ImGui::PushItemWidth(5); const char* returnValue = \"return\"; ImGui::PopItemWidth(); }",
                        "ImGui::PushItemWidth(")); // the word in a string or a longer name is not a return
}
} // namespace

int main()
{
    StripCommentsRules();
    FunctionBodyRules();
    StackRules();
    if (failures)
    {
        std::printf("menu_scan_self: %d failure(s)\n", failures);
        return 2;
    }
    std::printf("menu_scan_self: all passed\n");
    return 0;
}
