Scriptname FreeClimbMCM extends SKI_ConfigBase
{ FreeClimb LE - in-game menu (SkyUI MCM). Nothing is stored in this script: every option reads and
  writes straight to the FreeClimb DLL, which saves the .ini file. It also plays the climbing sounds
  when the DLL asks for them (mod event "FreeClimb_Sound"). }

; ---- table of the options shown on the current page (rebuilt on every page reset)
string[] okey
int[] oid
int[] okind     ; 1 = toggle, 2 = slider, 3 = menu, 5 = button, 6 = status
int[] oisint    ; slider: 1 = integer setting, 0 = float setting
int[] omenu     ; menu: which list of choices (see MenuChoices)
float[] omin
float[] omax
float[] ostep
float[] odef
float[] oscale  ; shown value = real value * scale (e.g. 0..1 shown as percent)
string[] ofmt
string[] oinfo
int ocount
int lang        ; 0 = English, 1 = Portugues
int soundSeen   ; how many sound events were received (only the first few are logged)

; ---- climbing sounds (looked up once, on first use)
Sound sndStep
Sound sndGrip
Sound sndPush
Sound sndTop

int Function GetVersion()
    return 1
EndFunction

Function InitTables()
    okey = new string[64]
    oid = new int[64]
    okind = new int[64]
    oisint = new int[64]
    omenu = new int[64]
    omin = new float[64]
    omax = new float[64]
    ostep = new float[64]
    odef = new float[64]
    oscale = new float[64]
    ofmt = new string[64]
    oinfo = new string[64]
    ocount = 0
EndFunction

Event OnConfigInit()
    Debug.Trace("FreeClimbMCM: OnConfigInit - the menu script is running")
    ModName = "FreeClimb"
    InitTables()
    SetPageNames()
    RegisterSoundEvent()
    Debug.Trace("FreeClimbMCM: menu registered as 'FreeClimb' with " + (Pages.Length as string) + " pages")
EndEvent

Event OnVersionUpdate(int a_version)
    InitTables()
    SetPageNames()
    RegisterSoundEvent()
EndEvent

Event OnConfigOpen()
    Debug.Trace("FreeClimbMCM: menu opened")
    InitTables()
    SetPageNames()
    RegisterSoundEvent()
EndEvent

Function RegisterSoundEvent()
    RegisterForModEvent("FreeClimb_Sound", "OnFreeClimbSound")
EndFunction

; ---- sounds: the DLL sends the event; the SOUN records live in FreeClimbMenu.esp (they wrap the FC_* descriptors of FreeClimb.esp)
Event OnFreeClimbSound(string a_eventName, string a_name, float a_volume, Form a_sender)
    if soundSeen < 3
        soundSeen += 1
        Debug.Trace("FreeClimbMCM: sound event '" + a_name + "' received")
    endIf
    Sound s = None
    if a_name == "step"
        if sndStep == None
            sndStep = Game.GetFormFromFile(0x801, "FreeClimbMenu.esp") as Sound
        endIf
        s = sndStep
    elseIf a_name == "grip"
        if sndGrip == None
            sndGrip = Game.GetFormFromFile(0x802, "FreeClimbMenu.esp") as Sound
        endIf
        s = sndGrip
    elseIf a_name == "push"
        if sndPush == None
            sndPush = Game.GetFormFromFile(0x803, "FreeClimbMenu.esp") as Sound
        endIf
        s = sndPush
    elseIf a_name == "top"
        if sndTop == None
            sndTop = Game.GetFormFromFile(0x804, "FreeClimbMenu.esp") as Sound
        endIf
        s = sndTop
    endIf
    if s == None
        Debug.Trace("FreeClimbMCM: sound marker for '" + a_name + "' was not found in FreeClimbMenu.esp")
        return
    endIf
    ObjectReference src = a_sender as ObjectReference
    if src == None
        src = Game.GetPlayer()
    endIf
    int h = s.Play(src)
    if h == 0 && soundSeen < 4
        Debug.Trace("FreeClimbMCM: Sound.Play returned 0 for '" + a_name + "' (the sound descriptor may be missing or silent)")
    endIf
    if h != 0 && a_volume < 0.99
        Sound.SetInstanceVolume(h, a_volume)
    endIf
EndEvent

; ---- language helper
string Function T(string en, string pt)
    if lang == 1
        return pt
    endIf
    return en
EndFunction

; Page names follow the chosen language (they refresh the next time the menu is opened).
Function SetPageNames()
    lang = FreeClimbNative.GetInt("Language")
    Pages = new string[7]
    Pages[0] = T("General", "Geral")
    Pages[1] = T("Movement", "Movimento")
    Pages[2] = T("Wall & entry", "Parede e entrada")
    Pages[3] = T("Smoothness", "Suavidade")
    Pages[4] = T("Stamina", "Estamina")
    Pages[5] = T("Surface & FPS", "Superficie e FPS")
    Pages[6] = T("Sounds", "Sons")
EndFunction

string Function ChoiceText(int menuId, int index)
    if menuId == 1
        if index == 1
            return "Portugues"
        endIf
        return "English"
    elseIf menuId == 2
        if index == 1
            return T("Jump + W + A + D together", "Salto + W + A + D juntos")
        endIf
        return T("Jump alone", "Salto sozinho")
    endIf
    return ""
EndFunction

string[] Function MenuChoices(int menuId)
    string[] c = new string[2]
    c[0] = ChoiceText(menuId, 0)
    c[1] = ChoiceText(menuId, 1)
    return c
EndFunction

; ---- rows
Function AddToggleRow(string key, string label, float def, string info)
    if ocount >= 64
        return
    endIf
    int i = ocount
    ocount += 1
    okey[i] = key
    okind[i] = 1
    odef[i] = def
    oinfo[i] = info
    oid[i] = AddToggleOption(label, FreeClimbNative.GetBool(key))
EndFunction

Function AddSliderRow(string key, string label, float minV, float maxV, float stepV, float def, float scale, int isInt, string fmt, string info)
    if ocount >= 64
        return
    endIf
    int i = ocount
    ocount += 1
    okey[i] = key
    okind[i] = 2
    oisint[i] = isInt
    omin[i] = minV
    omax[i] = maxV
    ostep[i] = stepV
    odef[i] = def
    oscale[i] = scale
    ofmt[i] = fmt
    oinfo[i] = info
    oid[i] = AddSliderOption(label, ShownValue(i), fmt)
EndFunction

Function AddMenuRow(string key, string label, int menuId, float def, string info)
    if ocount >= 64
        return
    endIf
    int i = ocount
    ocount += 1
    okey[i] = key
    okind[i] = 3
    omenu[i] = menuId
    odef[i] = def
    oinfo[i] = info
    oid[i] = AddMenuOption(label, ChoiceText(menuId, FreeClimbNative.GetInt(key)))
EndFunction

Function AddButtonRow(string key, string label, string btn, string info)
    if ocount >= 64
        return
    endIf
    int i = ocount
    ocount += 1
    okey[i] = key
    okind[i] = 5
    oinfo[i] = info
    oid[i] = AddTextOption(label, btn)
EndFunction

Function AddStatusRow(string label, string value, string info)
    if ocount >= 64
        return
    endIf
    int i = ocount
    ocount += 1
    okey[i] = ""
    okind[i] = 6
    oinfo[i] = info
    oid[i] = AddTextOption(label, value, OPTION_FLAG_DISABLED)
EndFunction

float Function ShownValue(int i)
    float v
    if oisint[i] == 1
        v = FreeClimbNative.GetInt(okey[i]) as float
    else
        v = FreeClimbNative.GetFloat(okey[i])
    endIf
    return v * oscale[i]
EndFunction

int Function FindRow(int a_option)
    int i = 0
    while i < ocount
        if oid[i] == a_option
            return i
        endIf
        i += 1
    endWhile
    return -1
EndFunction

Function StoreSlider(int i, float shown)
    float raw = shown / oscale[i]
    if oisint[i] == 1
        FreeClimbNative.SetInt(okey[i], (raw + 0.5) as int)
    else
        FreeClimbNative.SetFloat(okey[i], raw)
    endIf
EndFunction

string Function YesNo(int v)
    if v != 0
        return T("yes", "sim")
    endIf
    return T("no", "nao")
EndFunction

; ---- pages
Event OnPageReset(string a_page)
    InitTables()
    lang = FreeClimbNative.GetInt("Language")

    int p = 0
    if a_page != ""
        p = Pages.Find(a_page)
        if p < 0
            p = 0
        endIf
    endIf

    if p == 0
        PageGeneral()
    elseIf p == 1
        PageMovement()
    elseIf p == 2
        PageWall()
    elseIf p == 3
        PageSmooth()
    elseIf p == 4
        PageStamina()
    elseIf p == 5
        PageSurface()
    else
        PageSounds()
    endIf
EndEvent

Function PageGeneral()
    AddHeaderOption("FreeClimb LE")
    AddEmptyOption()
    AddMenuRow("Language", T("Menu language", "Idioma do menu"), 1, 0.0, T("Language of this menu. Page names update the next time you open the menu.", "Idioma deste menu. Os nomes das paginas mudam na proxima vez que abrir o menu."))
    AddEmptyOption()
    AddHeaderOption(T("Status", "Estado"))
    AddEmptyOption()
    AddStatusRow(T("Animation pack loaded", "Pacote de animacoes carregado"), YesNo(FreeClimbNative.Status(0)), T("If this says no, the animations are missing or the pack is the wrong version (use the pack that matches this DLL).", "Se disser nao, faltam as animacoes ou o pacote e de outra versao."))
    AddStatusRow(T("Skeleton bones bound", "Ossos do esqueleto ligados"), FreeClimbNative.Status(1) as string, T("Number of bones (of 99) the mod controls on your character. About 80 is normal.", "Numero de ossos (de 99) que o mod controla no seu personagem. Cerca de 80 e normal."))
    AddStatusRow(T("Climbing now", "A escalar agora"), YesNo(FreeClimbNative.Status(2)), "")
    AddStatusRow(T("Stamina", "Estamina"), (FreeClimbNative.Status(3) as string) + " %", T("Your current stamina, in percent of the maximum.", "A sua estamina atual, em percentagem do maximo."))
    AddEmptyOption()
    AddHeaderOption(T("Settings file", "Ficheiro de configuracao"))
    AddEmptyOption()
    AddButtonRow("@save", T("Save my profile", "Guardar o meu perfil"), T("[Save]", "[Guardar]"), T("Saves all current options as your own profile, to load again later.", "Guarda todas as opcoes atuais como o seu perfil, para carregar mais tarde."))
    AddButtonRow("@load", T("Load my profile", "Carregar o meu perfil"), T("[Load]", "[Carregar]"), T("Replaces all current options with the ones you saved.", "Substitui todas as opcoes atuais pelas que guardou."))
    AddButtonRow("@reset", T("Reset to defaults", "Repor valores de fabrica"), T("[Reset]", "[Repor]"), T("Restores all options to the factory values.", "Repoe todas as opcoes para os valores de fabrica."))
    AddEmptyOption()
    AddHeaderOption(T("Diagnostics", "Diagnostico"))
    AddEmptyOption()
    AddToggleRow("Verbose", T("Detailed log", "Registo detalhado"), 0.0, T("Writes extra detail to FreeClimbLE.log. Makes the log big; use only when asked.", "Escreve mais detalhe em FreeClimbLE.log. O log fica grande; use so quando pedido."))
EndFunction

Function PageMovement()
    AddHeaderOption(T("Speed (0 = automatic, the clip's own speed)", "Velocidade (0 = automatica, a do proprio clip)"))
    AddEmptyOption()
    AddSliderRow("ClimbSpeed", T("Climb speed", "Velocidade a subir"), 0.0, 300.0, 5.0, 0.0, 1.0, 0, "{0}", T("0 = automatic (about 45). The original mod uses 100.", "0 = automatica (cerca de 45). O mod original usa 100."))
    AddSliderRow("DownSpeed", T("Descend speed", "Velocidade a descer"), 0.0, 300.0, 5.0, 0.0, 1.0, 0, "{0}", T("0 = automatic. The original mod uses 78.", "0 = automatica. O mod original usa 78."))
    AddSliderRow("SideSpeed", T("Sideways speed", "Velocidade de lado"), 0.0, 300.0, 5.0, 0.0, 1.0, 0, "{0}", T("0 = automatic (about 36). The original mod uses 82.", "0 = automatica (cerca de 36). O mod original usa 82."))
    AddSliderRow("SpeedScale", T("Speed multiplier", "Multiplicador de velocidade"), 0.25, 3.0, 0.05, 1.0, 1.0, 0, "{2}x", T("Multiplies every automatic speed.", "Multiplica todas as velocidades automaticas."))
    AddSliderRow("StartSpeed", T("Start speed", "Velocidade ao arrancar"), 0.05, 1.0, 0.05, 0.45, 100.0, 0, "{0}%", T("Fraction of the speed when you start moving. 100% turns the ramp off.", "Fracao da velocidade ao comecar a mexer. 100% desliga a rampa."))
    AddSliderRow("RampSeconds", T("Ramp-up time", "Tempo da rampa"), 0.0, 3.0, 0.1, 0.7, 1.0, 0, "{1} s", T("Seconds to reach full speed.", "Segundos ate a velocidade completa."))
    AddEmptyOption()
    AddHeaderOption(T("Wall run (Shift)", "Corrida na parede (Shift)"))
    AddEmptyOption()
    AddSliderRow("WallRunSpeed", T("Wall run speed", "Velocidade da corrida"), 100.0, 800.0, 10.0, 379.5, 1.0, 0, "{0}", "")
    AddSliderRow("WallRunDiagonal", T("Diagonal multiplier", "Multiplicador na diagonal"), 0.5, 2.0, 0.05, 1.15, 1.0, 0, "{2}x", "")
    AddSliderRow("RunHeight", T("Body height while running", "Altura do corpo na corrida"), -80.0, 120.0, 5.0, 0.0, 1.0, 0, "{0}", T("Raises or lowers the body while running on the wall.", "Sobe ou desce o corpo durante a corrida na parede."))
    AddEmptyOption()
    AddHeaderOption(T("Jumps", "Saltos"))
    AddEmptyOption()
    AddSliderRow("HopScale", T("Hop distance", "Distancia do salto"), 0.5, 4.0, 0.1, 1.8, 1.0, 0, "{1}x", "")
    AddSliderRow("HopSpeed", T("Hop speed", "Velocidade do salto"), 0.5, 3.0, 0.05, 1.1, 1.0, 0, "{2}x", "")
    AddToggleRow("HopEase", T("Smooth hops", "Saltos suaves"), 1.0, T("Hops start and end gently.", "Os saltos comecam e acabam devagar."))
    AddToggleRow("HopProbe", T("Read the wall before hopping", "Ler a parede antes de saltar"), 1.0, T("Follows the wall (also around pillars and trees). With no wall ahead, the hop is a normal jump and you fall.", "Segue a parede (tambem em pilares e arvores). Sem parede a frente, o salto e normal e voce cai."))
    AddToggleRow("ContextHops", T("Hop to the next wall", "Saltar para a parede seguinte"), 1.0, T("Hops by itself to the next wall when this one ends.", "Salta sozinho para a parede seguinte quando esta acaba."))
    AddSliderRow("CatchSpeed", T("Grab animation speed", "Velocidade de agarrar"), 0.5, 3.0, 0.1, 1.5, 1.0, 0, "{1}x", "")
    AddEmptyOption()
    AddHeaderOption(T("Slide down (Shift + S)", "Escorregar (Shift + S)"))
    AddEmptyOption()
    AddToggleRow("SlideEnabled", T("Slide with Shift + S", "Escorregar com Shift + S"), 1.0, T("Hold Shift + S on the wall to slide down, speeding up. Let go to brake and keep hanging. At the ground you land standing.", "Segure Shift + S na parede para escorregar, acelerando. Largue para travar e ficar agarrado. No chao, aterra de pe."))
    AddSliderRow("SlideMaxSpeed", T("Slide top speed", "Velocidade maxima"), 50.0, 600.0, 10.0, 280.0, 1.0, 0, "{0}", "")
    AddSliderRow("SlideAccel", T("Slide acceleration", "Aceleracao"), 50.0, 1200.0, 10.0, 220.0, 1.0, 0, "{0}", T("How fast it speeds up. A/D steers at 40% speed while sliding.", "Quao depressa acelera. O A/D corrige a trajetoria a 40% da velocidade."))
    AddSliderRow("SlideStamina", T("Stamina cost", "Custo de estamina"), 0.0, 30.0, 0.5, 1.0, 1.0, 0, "{1} %/s", "")
EndFunction

Function PageWall()
    AddHeaderOption(T("Grabbing the wall", "Agarrar a parede"))
    AddEmptyOption()
    AddMenuRow("EntryMode", T("How to grab", "Como agarrar"), 2, 0.0, T("Jump alone next to a wall, or Jump + W + A + D together (like the original).", "Salto sozinho junto a uma parede, ou Salto + W + A + D juntos (como o original)."))
    AddToggleRow("AlwaysCling", T("Always cling", "Agarrar sempre"), 1.0, T("Jumping next to a wall always clings; off = low walls are vaulted instead.", "Saltar junto a uma parede agarra sempre; desligado = paredes baixas sao passadas por cima."))
    AddSliderRow("Reach", T("Search reach", "Alcance da procura"), 30.0, 300.0, 5.0, 110.0, 1.0, 0, "{0}", T("How far it looks for a wall when you jump.", "Ate onde procura a parede quando salta."))
    AddSliderRow("AttachReach", T("Attach distance", "Distancia para agarrar"), 10.0, 200.0, 5.0, 65.0, 1.0, 0, "{0}", "")
    AddSliderRow("AirReach", T("Mid-air reach", "Alcance no ar"), 10.0, 300.0, 5.0, 90.0, 1.0, 0, "{0}", T("Reach to catch a wall in the middle of a jump.", "Alcance para apanhar uma parede a meio de um salto."))
    AddEmptyOption()
    AddHeaderOption(T("On the wall", "Na parede"))
    AddEmptyOption()
    AddSliderRow("WallGap", T("Distance to the wall", "Distancia a parede"), 10.0, 80.0, 1.0, 30.0, 1.0, 0, "{0}", T("30 is the original's. Raise it 2-3 if the hands go into the wall.", "30 e o do original. Suba 2-3 se as maos entrarem na parede."))
    AddToggleRow("FaceWall", T("Face the wall", "Virar para a parede"), 1.0, "")
    AddToggleRow("Warp", T("Warp every update", "Teletransportar a cada atualizacao"), 1.0, T("Keep on unless you know why.", "Deixe ligado a nao ser que saiba porque."))
    AddEmptyOption()
    AddHeaderOption(T("Ledges", "Bordas"))
    AddEmptyOption()
    AddSliderRow("MinLedge", T("Minimum ledge height", "Altura minima da borda"), 20.0, 300.0, 5.0, 80.0, 1.0, 0, "{0}", "")
    AddSliderRow("MaxLedge", T("Maximum ledge height", "Altura maxima da borda"), 40.0, 400.0, 5.0, 180.0, 1.0, 0, "{0}", "")
    AddSliderRow("HeadHeight", T("Head height", "Altura da cabeca"), 60.0, 250.0, 5.0, 125.0, 1.0, 0, "{0}", T("Where it looks for the top of the wall.", "Onde procura o topo da parede."))
EndFunction

Function PageSmooth()
    AddHeaderOption(T("Turning and position", "Rotacao e posicao"))
    AddEmptyOption()
    AddSliderRow("TurnSmooth", T("Turn smoothness", "Suavidade da rotacao"), 0.0, 0.6, 0.01, 0.15, 1.0, 0, "{2} s", T("Higher = softer and slower. 0 = instant. Works around pillars, trees and corners.", "Maior = mais suave e lento. 0 = instantaneo. Funciona em pilares, arvores e cantos."))
    AddSliderRow("TurnSpeed", T("Maximum turn speed", "Velocidade maxima de rotacao"), 60.0, 1440.0, 30.0, 540.0, 1.0, 0, "{0} deg/s", "")
    AddSliderRow("PosSmooth", T("Position smoothness", "Suavidade da posicao"), 0.0, 0.5, 0.01, 0.10, 1.0, 0, "{2} s", T("Softens the move when the wall changes direction.", "Suaviza o movimento quando a parede muda de direcao."))
    AddEmptyOption()
    AddHeaderOption(T("Animation blending", "Fusao de animacoes"))
    AddEmptyOption()
    AddSliderRow("EntryBlend", T("Blend in", "Fusao ao entrar"), 0.0, 1.0, 0.01, 0.14, 1.0, 0, "{2} s", "")
    AddSliderRow("ExitBlend", T("Blend out", "Fusao ao sair"), 0.0, 1.5, 0.01, 0.22, 1.0, 0, "{2} s", "")
    AddSliderRow("MotionBlend", T("Blend between moves", "Fusao entre movimentos"), 0.0, 1.0, 0.01, 0.15, 1.0, 0, "{2} s", "")
    AddEmptyOption()
    AddHeaderOption(T("Pose engine", "Motor de pose"))
    AddEmptyOption()
    AddToggleRow("PoseMode", T("Write pose before the game update", "Escrever a pose antes da atualizacao"), 1.0, T("Recommended. Off = old method (only for comparing).", "Recomendado. Desligado = metodo antigo (so para comparar)."))
EndFunction

Function PageStamina()
    AddHeaderOption(T("Stamina (percent of maximum)", "Estamina (percentagem do maximo)"))
    AddEmptyOption()
    AddToggleRow("StaminaEnabled", T("Climbing costs stamina", "Escalar gasta estamina"), 1.0, "")
    AddSliderRow("StaminaScale", T("Difficulty multiplier", "Multiplicador de dificuldade"), 0.1, 5.0, 0.1, 1.0, 1.0, 0, "{1}x", T("Multiplies every cost below.", "Multiplica todos os custos abaixo."))
    AddToggleRow("StaminaBlockRegen", T("No regeneration on the wall", "Sem regeneracao na parede"), 1.0, T("Stamina does not regenerate while you are on the wall (it does while you slide, so you can recover).", "A estamina nao regenera enquanto esta na parede (regenera enquanto escorrega, para recuperar)."))
    AddEmptyOption()
    AddHeaderOption(T("Cost per second", "Custo por segundo"))
    AddEmptyOption()
    AddSliderRow("StaminaHang", T("Hanging still", "Parado agarrado"), 0.0, 30.0, 0.5, 1.0, 1.0, 0, "{1} %/s", "")
    AddSliderRow("StaminaClimb", T("Climbing", "A subir"), 0.0, 30.0, 0.5, 3.0, 1.0, 0, "{1} %/s", "")
    AddSliderRow("StaminaDown", T("Descending", "A descer"), 0.0, 30.0, 0.5, 1.5, 1.0, 0, "{1} %/s", "")
    AddSliderRow("StaminaSide", T("Sideways", "De lado"), 0.0, 30.0, 0.5, 3.0, 1.0, 0, "{1} %/s", "")
    AddSliderRow("StaminaRun", T("Wall running", "Corrida na parede"), 0.0, 50.0, 0.5, 8.0, 1.0, 0, "{1} %/s", "")
    AddEmptyOption()
    AddHeaderOption(T("Cost per action", "Custo por acao"))
    AddEmptyOption()
    AddSliderRow("StaminaHop", T("Each hop", "Cada salto"), 0.0, 60.0, 1.0, 6.0, 1.0, 0, "{0} %", "")
    AddSliderRow("StaminaMantle", T("Pulling over a ledge", "Passar a borda"), 0.0, 60.0, 1.0, 4.0, 1.0, 0, "{0} %", "")
    AddEmptyOption()
    AddHeaderOption(T("When it runs out", "Quando acaba"))
    AddEmptyOption()
    AddSliderRow("StaminaSlideSpeed", T("Slide speed", "Velocidade a escorregar"), 0.0, 120.0, 5.0, 25.0, 1.0, 0, "{0}", T("With no stamina you slide down slowly, until you reach the ground.", "Sem estamina escorrega devagar para baixo, ate chegar ao chao."))
    AddSliderRow("StaminaResume", T("Can climb again at", "Volta a poder subir com"), 0.0, 100.0, 5.0, 15.0, 1.0, 0, "{0} %", T("Stamina needed (in percent) before climbing works again.", "Estamina necessaria (em percentagem) para voltar a escalar."))
EndFunction

Function PageSurface()
    AddHeaderOption(T("Hands and feet on the surface", "Maos e pes na superficie"))
    AddEmptyOption()
    AddToggleRow("SurfaceFit", T("Fit to the real surface", "Ajustar a superficie real"), 1.0, T("Hands and feet follow trees, rocks and uneven walls. Costs a few rays per update.", "Maos e pes seguem arvores, rochas e paredes irregulares. Custa alguns raios por atualizacao."))
    AddSliderRow("FitRays", T("Ray budget", "Orcamento de raios"), 1.0, 12.0, 1.0, 4.0, 1.0, 1, "{0}", T("More = more precise but heavier. Lower this if your FPS drops.", "Mais = mais preciso mas mais pesado. Baixe se o seu FPS cair."))
    AddSliderRow("FitMinFps", T("Lighten below FPS", "Aliviar abaixo de FPS"), 10.0, 120.0, 5.0, 35.0, 1.0, 0, "{0}", T("Below this frame rate it uses fewer rays by itself.", "Abaixo desta taxa usa menos raios sozinho."))
    AddEmptyOption()
    AddHeaderOption(T("Gamepad", "Comando"))
    AddEmptyOption()
    AddToggleRow("AnalogEnabled", T("Analog stick", "Analogico"), 1.0, T("Continuous direction and speed with the left stick.", "Direcao e velocidade continuas com o analogico esquerdo."))
    AddSliderRow("AnalogDeadZone", T("Stick dead zone", "Zona morta do analogico"), 0.0, 0.8, 0.05, 0.25, 100.0, 0, "{0}%", "")
EndFunction

Function PageSounds()
    AddHeaderOption(T("Climbing sounds", "Sons da escalada"))
    AddEmptyOption()
    AddToggleRow("SoundEnabled", T("Play climbing sounds", "Tocar os sons da escalada"), 1.0, T("Grab, steps, push-off and pulling over a ledge.", "Agarrar, passos, impulso e passar a borda."))
    AddSliderRow("SoundVolume", T("Volume", "Volume"), 0.0, 1.0, 0.05, 1.0, 100.0, 0, "{0}%", "")
    AddSliderRow("SoundGapMs", T("Minimum gap between repeats", "Intervalo minimo entre repeticoes"), 0.0, 1000.0, 10.0, 140.0, 1.0, 0, "{0} ms", "")
EndFunction

; ---- events
Event OnOptionSelect(int a_option)
    int i = FindRow(a_option)
    if i < 0
        return
    endIf
    if okind[i] == 1
        bool v = !FreeClimbNative.GetBool(okey[i])
        FreeClimbNative.SetBool(okey[i], v)
        SetToggleOptionValue(a_option, v)
    elseIf okind[i] == 5
        if okey[i] == "@save"
            FreeClimbNative.SaveProfile()
            SetTextOptionValue(a_option, T("[Saved]", "[Guardado]"))
        elseIf okey[i] == "@load"
            if FreeClimbNative.LoadProfile() == 1
                ForcePageReset()
            else
                ShowMessage(T("No saved profile found. Use 'Save my profile' first.", "Nenhum perfil guardado. Use primeiro 'Guardar o meu perfil'."), false)
            endIf
        elseIf okey[i] == "@reset"
            if ShowMessage(T("Restore all FreeClimb options to the factory values?", "Repor todas as opcoes do FreeClimb para os valores de fabrica?"), true)
                FreeClimbNative.ResetDefaults()
                ForcePageReset()
            endIf
        endIf
    endIf
EndEvent

Event OnOptionDefault(int a_option)
    int i = FindRow(a_option)
    if i < 0
        return
    endIf
    if okind[i] == 1
        bool v = odef[i] != 0.0
        FreeClimbNative.SetBool(okey[i], v)
        SetToggleOptionValue(a_option, v)
    elseIf okind[i] == 2
        StoreSlider(i, odef[i] * oscale[i])
        SetSliderOptionValue(a_option, ShownValue(i), ofmt[i])
    elseIf okind[i] == 3
        FreeClimbNative.SetInt(okey[i], odef[i] as int)
        SetMenuOptionValue(a_option, ChoiceText(omenu[i], odef[i] as int))
    endIf
EndEvent

Event OnOptionSliderOpen(int a_option)
    int i = FindRow(a_option)
    if i < 0
        return
    endIf
    SetSliderDialogStartValue(ShownValue(i))
    SetSliderDialogDefaultValue(odef[i] * oscale[i])
    SetSliderDialogRange(omin[i] * oscale[i], omax[i] * oscale[i])
    SetSliderDialogInterval(ostep[i] * oscale[i])
EndEvent

Event OnOptionSliderAccept(int a_option, float a_value)
    int i = FindRow(a_option)
    if i < 0
        return
    endIf
    StoreSlider(i, a_value)
    SetSliderOptionValue(a_option, ShownValue(i), ofmt[i])
EndEvent

Event OnOptionMenuOpen(int a_option)
    int i = FindRow(a_option)
    if i < 0
        return
    endIf
    SetMenuDialogOptions(MenuChoices(omenu[i]))
    SetMenuDialogStartIndex(FreeClimbNative.GetInt(okey[i]))
    SetMenuDialogDefaultIndex(odef[i] as int)
EndEvent

Event OnOptionMenuAccept(int a_option, int a_index)
    int i = FindRow(a_option)
    if i < 0
        return
    endIf
    FreeClimbNative.SetInt(okey[i], a_index)
    if okey[i] == "Language"
        lang = a_index
        ForcePageReset()
    else
        SetMenuOptionValue(a_option, ChoiceText(omenu[i], a_index))
    endIf
EndEvent

Event OnOptionHighlight(int a_option)
    int i = FindRow(a_option)
    if i >= 0
        SetInfoText(oinfo[i])
    endIf
EndEvent
