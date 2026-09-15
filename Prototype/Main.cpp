// Main.cpp — 프로토타입 테스트 러너
// 사용법: PrototypeTests.exe [이름 필터]   (필터 문자열이 포함된 테스트만 실행)
#include "TestFramework.h"
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

int main(int Argc, char** Argv)
{
#ifdef _WIN32
	SetConsoleOutputCP(CP_UTF8); // /utf-8로 컴파일한 한글 로그가 깨지지 않도록
#endif
	const char* Filter = Argc > 1 ? Argv[1] : nullptr;

	int Passed = 0;
	int Failed = 0;
	for (const TestFramework::FTestCase& Test : TestFramework::Registry())
	{
		if (Filter && !std::strstr(Test.Name, Filter)) continue;

		std::printf("[ RUN  ] %s\n", Test.Name);
		TestFramework::CurrentFailures() = 0;
		Test.Fn();

		const bool bOk = TestFramework::CurrentFailures() == 0;
		std::printf("[ %s ] %s\n", bOk ? "PASS" : "FAIL", Test.Name);
		(bOk ? Passed : Failed)++;
	}

	std::printf("\n%d passed, %d failed\n", Passed, Failed);
	return Failed == 0 ? 0 : 1;
}
