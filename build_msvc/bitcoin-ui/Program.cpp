#include "MainForm.h"

using namespace System;
using namespace Windows::Forms;
using namespace Bitcoin;

[STAThreadAttribute]
int main(array<String^>^ args)
{
	Application::EnableVisualStyles();
	Application::SetCompatibleTextRenderingDefault(false);
	MainForm form;
	Application::Run(%form);
}
