// TestFramework.h
// ---------------------------------------------------------
// 외부 의존성 없는 초소형 테스트 프레임워크 (프로토타입 검증용).
// ---------------------------------------------------------
#pragma once

#include <cmath>
#include <cstdio>
#include <vector>

namespace TestFramework
{
	struct FTestCase
	{
		const char* Name;
		void (*Fn)();
	};

	inline std::vector<FTestCase>& Registry()
	{
		static std::vector<FTestCase> Tests;
		return Tests;
	}

	inline int& CurrentFailures()
	{
		static int Failures = 0;
		return Failures;
	}

	struct FRegistrar
	{
		FRegistrar(const char* Name, void (*Fn)()) { Registry().push_back({ Name, Fn }); }
	};

	inline void ReportFailure(const char* File, int Line, const char* Expr, const char* Detail = "")
	{
		++CurrentFailures();
		std::printf("    FAIL %s(%d): %s %s\n", File, Line, Expr, Detail);
	}
}

#define TEST_CASE(Name) \
	static void Name(); \
	static TestFramework::FRegistrar Name##_Registrar(#Name, &Name); \
	static void Name()

#define CHECK(Cond) \
	do { if (!(Cond)) TestFramework::ReportFailure(__FILE__, __LINE__, #Cond); } while (0)

#define CHECK_NEAR(Actual, Expected, Tolerance) \
	do { \
		const double A_ = (Actual), E_ = (Expected), T_ = (Tolerance); \
		if (!(std::fabs(A_ - E_) <= T_)) { \
			char Buf_[192]; \
			std::snprintf(Buf_, sizeof(Buf_), "(actual=%.6g expected=%.6g tol=%.3g)", A_, E_, T_); \
			TestFramework::ReportFailure(__FILE__, __LINE__, #Actual " ~= " #Expected, Buf_); \
		} \
	} while (0)

#define LOG(...) \
	do { std::printf("    "); std::printf(__VA_ARGS__); std::printf("\n"); } while (0)
