Scriptname SKI_ConfigBase extends SKI_QuestBase

string Property ModName Auto
string[] Property Pages Auto

int Property OPTION_FLAG_NONE = 0 AutoReadOnly
int Property OPTION_FLAG_DISABLED = 1 AutoReadOnly
int Property OPTION_FLAG_HIDDEN = 2 AutoReadOnly
int Property OPTION_FLAG_WITH_UNMAP = 4 AutoReadOnly

Event OnConfigInit()
EndEvent
Event OnConfigOpen()
EndEvent
Event OnConfigClose()
EndEvent
Event OnPageReset(string a_page)
EndEvent
Event OnOptionSelect(int a_option)
EndEvent
Event OnOptionDefault(int a_option)
EndEvent
Event OnOptionHighlight(int a_option)
EndEvent
Event OnOptionSliderOpen(int a_option)
EndEvent
Event OnOptionSliderAccept(int a_option, float a_value)
EndEvent
Event OnOptionMenuOpen(int a_option)
EndEvent
Event OnOptionMenuAccept(int a_option, int a_index)
EndEvent
Event OnOptionKeyMapChange(int a_option, int a_keyCode, string a_conflictControl, string a_conflictName)
EndEvent

Function ForcePageReset()
EndFunction
Function SetInfoText(string a_text)
EndFunction
int Function AddHeaderOption(string a_text, int a_flags = 0)
	return 0
EndFunction
int Function AddEmptyOption()
	return 0
EndFunction
int Function AddToggleOption(string a_text, bool a_checked, int a_flags = 0)
	return 0
EndFunction
int Function AddTextOption(string a_text, string a_value, int a_flags = 0)
	return 0
EndFunction
int Function AddSliderOption(string a_text, float a_value, string a_formatString = "{0}", int a_flags = 0)
	return 0
EndFunction
int Function AddMenuOption(string a_text, string a_value, int a_flags = 0)
	return 0
EndFunction
int Function AddKeyMapOption(string a_text, int a_keyCode, int a_flags = 0)
	return 0
EndFunction
Function SetToggleOptionValue(int a_option, bool a_checked, bool a_noUpdate = false)
EndFunction
Function SetSliderOptionValue(int a_option, float a_value, string a_formatString = "{0}", bool a_noUpdate = false)
EndFunction
Function SetMenuOptionValue(int a_option, string a_value, bool a_noUpdate = false)
EndFunction
Function SetKeyMapOptionValue(int a_option, int a_keyCode, bool a_noUpdate = false)
EndFunction
Function SetSliderDialogStartValue(float a_value)
EndFunction
Function SetSliderDialogDefaultValue(float a_value)
EndFunction
Function SetSliderDialogRange(float a_minValue, float a_maxValue)
EndFunction
Function SetSliderDialogInterval(float a_value)
EndFunction
Function SetMenuDialogStartIndex(int a_value)
EndFunction
Function SetMenuDialogDefaultIndex(int a_value)
EndFunction
Function SetMenuDialogOptions(string[] a_options)
EndFunction
Function SetTextOptionValue(int a_option, string a_value, bool a_noUpdate = false)
EndFunction
bool Function ShowMessage(string a_message, bool a_withCancel = true, string a_acceptLabel = "$Accept", string a_cancelLabel = "$Cancel")
	return true
EndFunction
