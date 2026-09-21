// The real luaStoreValidate, compiled for the host against stubs, driven with
// the exact cases the audit used to break the first version.
#define LUA_STORE_ENABLED 1
#define LUA_EFFECTS_ENABLED 1
#include "../../src/lua/lua_store.cpp"
#include <string>
#include <vector>
#include <cstdio>

static int fails = 0;
static void check(const char *what, const std::string &src, bool wantOk) {
  char err[200] = {0};
  const bool got = luaStoreValidate(src.c_str(), src.size(), err, sizeof(err));
  const bool pass = (got == wantOk);
  if (!pass) fails++;
  printf("  %s %-52s -> %s%s%s\n", pass ? "ok  " : "ПЛОХО", what,
         got ? "ПРИНЯТ" : "отвергнут", got ? "" : ": ", got ? "" : err);
}
static std::string rep(const std::string &u, int n) { std::string s; for (int i=0;i<n;i++) s += u; return s; }

int main() {
  printf("предел вложенности: %d\n\n", LUA_USER_DEPTH_MAX);

  printf("-- должны проходить --\n");
  check("обычный скрипт", "function draw() px.clear(0,0,0) px.text(1,1,\"HI\",9,9,9) end", true);
  check("вложенные таблицы в пределах", "local t = {a={b={c={1,2,3}}}}\nfunction draw() end", true);
  check("длинная строка со скобками", "local s = [[ )))) (((( ]]\nfunction draw() end", true);
  check("уровневая длинная строка", "local s = [==[ ]] )))) ]==]\nfunction draw() end", true);
  check("комментарий со скобками", "-- (((( ))))\nfunction draw() end", true);
  check("длинный комментарий", "--[[ ( ( ( ]]\nfunction draw() end", true);
  check("уровневый длинный комментарий", "--[==[ ]] ((( ]==]\nfunction draw() end", true);
  check("for/while не считаются дважды",
        "function draw() for i=1,3 do while true do break end end end", true);
  check("if/elseif/else один уровень",
        "function draw() if a then x() elseif b then y() else z() end end", true);
  check("repeat/until", "function draw() repeat x() until true end", true);
  check("строка с экранированной кавычкой", "local s = \"a\\\"b((((\"\nfunction draw() end", true);

  printf("\n-- должны отвергаться (падение в сторону отказа) --\n");
  check("незакрытый длинный комментарий", "--[[ ((((\nfunction draw() end", false);
  check("незакрытая длинная строка", "local s = [[ ((((\nfunction draw() end", false);
  check("незакрытая кавычка", "local s = \"abc\nfunction draw() end", false);
  check("обход аудита: уровневая строка гасит счётчик",
        "function draw() " + rep("f(", 10) + " [=[ " + rep(")", 10) + " ]=] " +
        rep("f(", 30) + "1" + rep(")", 30) + rep(")", 10) + " end", false);
  check("сорок вложенных local function (без единой скобки)",
        "function draw() end\n" + rep("local function f() ", 40) + rep(" end", 40), false);
  check("сорок вложенных if", "function draw() " + rep("if a then ", 40) + rep(" end", 40) + " end", false);
  check("глубокие скобки", "function draw() local x = " + rep("(", 30) + "1" + rep(")", 30) + " end", false);
  check("лишняя закрывающая", "function draw() end)", false);
  check("лишний end", "function draw() end end", false);
  check("незакрытый блок", "function draw()", false);
  check("нет draw", "function other() end", false);
  check("пустой файл", "", false);
  check("слишком большой", std::string(LUA_USER_SRC_MAX + 1, 'x'), false);

  check("длинная строка закрыта последним байтом", "function draw() end\nlocal s = [[ hi ]]", true);
  check("длинный комментарий закрыт последним байтом", "function draw() end\n--[[ hi ]]", true);
  check("уровневая закрыта последним байтом", "function draw() end\nlocal s = [==[ hi ]==]", true);
  check("goto и метки", "function draw() ::top:: goto top end", true);
  check("CRLF", "function draw()\r\n  px.clear(0,0,0)\r\nend\r\n", true);
  check("end внутри строки не закрывает блок", "function draw() local s = \"end end end\" end", true);
  check("-- внутри длинной строки", "local s = [[ -- ]]\nfunction draw() end", true);
  check("]] внутри кавычек", "local s = \"]]\"\nfunction draw() end", true);
  check("function как выражение", "local f = function() end\nfunction draw() f() end", true);
  check("глубокий конструктор таблиц (худший цикл)",
        "function draw() local t = " + rep("{a=", 20) + "1" + rep("}", 20) + " end", false);

  printf("\n%s\n", fails ? "ЕСТЬ ПРОВАЛЫ" : "все случаи как задумано");
  return fails ? 1 : 0;
}
