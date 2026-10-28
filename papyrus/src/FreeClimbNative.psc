Scriptname FreeClimbNative Native Hidden
{ Native functions implemented by the FreeClimbLE DLL. The menu reads and writes options through
  these, and the DLL saves the .ini on every change, so the menu and the .ini never disagree. }

bool Function GetBool(string key) global native
Function SetBool(string key, bool value) global native
int Function GetInt(string key) global native
Function SetInt(string key, int value) global native
float Function GetFloat(string key) global native
Function SetFloat(string key, float value) global native

; Re-reads the .ini (after editing it by hand).
Function Reload() global native

; Writes every option to the .ini.
Function SaveAll() global native

; Restores the factory values (keeps the language) and saves.
Function ResetDefaults() global native

; 0 = animation pack loaded (1/0), 1 = skeleton bones bound, 2 = climbing now (1/0), 3 = stamina in percent
int Function Status(int what) global native

; Saves all current options as the user's own profile (FreeClimbLE_User.ini).
Function SaveProfile() global native

; Loads the user's profile over the current options. Returns 1 if a profile existed, 0 if not.
int Function LoadProfile() global native
