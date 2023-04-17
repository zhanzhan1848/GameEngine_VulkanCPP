#pragma once
#include "Test.h"

class Engine_Test : public Test
{
public :
	bool initialize() override;
	void run() override;
	void shutdown() override;
};