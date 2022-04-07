
void StackTraceSignal(bool (*report)(void* pc, void* arg), void* arg, void* _ucxt, int max_size);
int StackTrace(bool (*report)(void *pc, void* arg), void* arg, int max_size);
