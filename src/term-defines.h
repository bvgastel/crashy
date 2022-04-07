
#define TERMINAL_COLOR_RED "\033[1;31m"
#define TERMINAL_COLOR_GREEN "\033[1;32m"
#define TERMINAL_COLOR_YELLOW "\033[1;33m"
#define TERMINAL_COLOR_BLUE "\033[1;34m"
#define TERMINAL_FULL "\033[1;37m"
#define TERMINAL_DIM "\033[1;90m"
#define TERMINAL_UNDERLINE "\033[4m"
#define TERMINAL_UNDERLINE_RESET "\033[24m"
#define TERMINAL_RESET "\033[0m"

//#define BAR "◢◤◢◤◢◤◢◤◢◤◢◤◢◤◢◤"
#define BAR "=========="

#define SYMBOL_BULLET "~~> "
#define SYMBOL_LOG "<|> "
#define SYMBOL_CONTEXT "->> "
#define SYMBOL_COMMANDLINE ">>-" 

#define TERMINAL_BULLET      TERMINAL_COLOR_YELLOW SYMBOL_BULLET      TERMINAL_RESET
#define TERMINAL_LOG         TERMINAL_COLOR_BLUE   SYMBOL_LOG         TERMINAL_RESET
#define TERMINAL_CONTEXT     TERMINAL_COLOR_GREEN  SYMBOL_CONTEXT     TERMINAL_RESET
#define TERMINAL_COMMANDLINE TERMINAL_COLOR_RED    SYMBOL_COMMANDLINE TERMINAL_RESET
#define TERMINAL_ALIGN "    "
