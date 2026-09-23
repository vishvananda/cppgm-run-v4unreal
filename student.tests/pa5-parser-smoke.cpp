namespace api { typedef int number; }
template<class T> T identity(T value) { return value; }
int main() { api::number value = identity(3); return value; }
