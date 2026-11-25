"""Native controls and authored artwork for the existing MOSAICO frontend.

The controller owns visibility and values. No browser preview data is embedded.
All action/binding names form the exported frontend_bindings.json contract.
"""
from scene_common import ASCII_PRINTABLE, button, container, image, label, layer

P = 'matter_bridge_frontend_'
ASSETS = 'frontend_assets/'
BG, CARD, MUTED, WHITE, ORANGE = '#000000', '#181819', '#91919B', '#FCFCFF', '#FF4C01'
DYNAMIC_ICONS = []
DEVICE_SLOTS = []
MARQUEE_LABELS = []


def add_frontend(objs, parent):
    DYNAMIC_ICONS.clear()
    DEVICE_SLOTS.clear()
    MARQUEE_LABELS.clear()
    def group(p, name, x=0, y=0, w=480, h=480, hidden=True):
        i = len(objs)
        objs.append(layer(p, x, y, w, h, name=P+name, bind=P+name+'_visible', hidden=hidden))
        return i

    def text(p, x, y, w, h, value, size=24, color=WHITE, binding=None, align='left', action=None):
        is_device_name = binding and (binding.endswith('_name') or binding.startswith('camera_name_'))
        if is_device_name:
            viewport=len(objs)
            objs.append(container(p,x,y,w,h,bg='#00000000',name=P+binding+'_viewport'))
            objs[-1]['clip_children']=True
            MARQUEE_LABELS.append({'binding':P+binding,'component_name':P+binding,'width':w,'font_size':size})
            p,x,y,w=viewport,0,0,1024
        objs.append(label(p, x,y,w,h,value,size=size,color=color,align=align,
                          bind=P+binding if binding else None,
                          name=P+binding if binding else None,
                          callback=P+action if action else None,
                          font_charset=ASCII_PRINTABLE if binding else None))
        if is_device_name:
            objs[-1]['overflow']='clip'
            objs[-1]['x'] = {'default': 0, 'min': objs[viewport]['w'] - w, 'max': 0}

    def box(p,x,y,w,h,bg=CARD,r=12,action=None):
        objs.append(container(p,x,y,w,h,bg=bg,radius=r,callback=P+action if action else None))
        return len(objs)-1

    def art(p,name,x,y,w,h,action=None,binding=None):
        objs.append(image(p,ASSETS+name+'.png',x,y,w,h,
                          callback=P+action if action else None,
                          name=P+binding if binding else None,
                          bind=P+binding if binding else None))

    def btn(p,x,y,w,h,value,action,bg=CARD,size=24,r=12):
        objs.append(button(p,x,y,w,h,value,bg=bg,size=size,radius=r,callback=P+action,
                           name=P+action+'_button_'+str(len(objs))))
        return len(objs)-1

    def iconbutton(p,name,x,y,w,h,action,bg=BG,r=0):
        btn(p,x,y,w,h,'',action,bg,r=r)
        art(p,name,x,y,w,h,action)

    def dynamic_art(p,kind,key,off,on,x,y,w,h,action):
        off_name=key+'_icon_off'
        on_name=key+'_icon_on'
        off_group=group(p,off_name,x,y,w,h,False)
        art(off_group,off,0,0,w,h,action)
        on_group=group(p,on_name,x,y,w,h,True)
        art(on_group,on,0,0,w,h,action)
        DYNAMIC_ICONS.append({'kind':kind,'on_binding':P+on_name+'_visible','off_binding':P+off_name+'_visible'})

    def dynamic_iconbutton(p,kind,key,off,on,x,y,w,h,action):
        btn(p,x,y,w,h,'',action,BG,r=0)
        dynamic_art(p,kind,key,off,on,x,y,w,h,action)

    def header(p,title,home=False,back='back'):
        box(p,0,0,480,64,BG,0)
        if home:
            box(p,24,25,14,14,ORANGE,0)
            text(p,46,15,180,36,'Home',24,'#D6D6DE')
            iconbutton(p,'info',408,8,48,48,'board_info')
        else:
            text(p,90,17,300,34,title,24,MUTED,align='center')
            iconbutton(p,'back',24,8,48,48,back)

    def feedback(p,name,y=460):
        text(p,12,y,456,20,'',14,ORANGE,name+'_feedback',align='center')

    def heading(p,y,suffix):
        text(p,12,y+6,220,44,'Device',36)
        text(p,248,y+14,152,30,'Turn All On',22,WHITE,'all_state_'+suffix,align='right',action='all_toggle')
        dynamic_iconbutton(p,'all','all_'+suffix,'on1','off1',412,y,56,56,'all_toggle')

    def automation(p,y,suffix):
        a=btn(p,12,y,456,88,'','automation_toggle',CARD,r=12)
        dynamic_art(a,'automation','automation_'+suffix,'automation','automation-on',12,16,56,56,'automation_toggle')
        text(a,80,10,210,30,'AUTOMATION',24,action='automation_toggle')
        text(a,322,10,122,32,'OFF',24,MUTED,'automation_state_'+suffix,align='right',action='automation_toggle')
        text(a,80,53,150,25,'LDR --',18,MUTED,'ldr_'+suffix,action='automation_toggle')
        text(a,238,53,170,25,'PIR --',18,MUTED,'pir_'+suffix,action='automation_toggle')

    def camera(p,y,suffix):
        text(p,12,y,440,43,'Monitor',36)
        c=box(p,12,y+51,456,177,CARD,12,'monitor_open')
        text(c,12,2,280,26,'',18,binding='camera_name_'+suffix,action='monitor_open')
        text(c,280,2,164,26,'Unavailable',16,MUTED,'monitor_state_'+suffix,align='right',action='monitor_open')
        live=group(c,'home_camera_live_'+suffix,280,0,176,29)
        box(live,0,0,164,29,CARD,0,'monitor_open')
        box(live,102,8,12,12,ORANGE,6,'monitor_open')
        text(live,122,0,42,29,'LIVE',18,ORANGE,align='right',action='monitor_open')
        box(c,1,29,454,147,BG,0,'monitor_open')
        art(c,'home_camera_canvas_'+suffix,1,29,454,147,'monitor_open','home_camera_canvas_'+suffix)
        objs[-1]['hidden']=True
        unavailable=group(c,'home_camera_unavailable_'+suffix,0,29,456,148,False)
        art(unavailable,'monitor-off',186,14,83,65,'monitor_open')
        text(unavailable,8,92,440,32,'CAMERA UNAVAILABLE',22,MUTED,align='center',action='monitor_open')
        off=group(c,'home_camera_off_'+suffix,1,29,454,147)
        box(off,0,0,454,147,CARD,0,'monitor_open')
        art(off,'monitor-off',185,19,83,65,'monitor_open')
        text(off,7,95,440,34,'MONITORING OFF',24,MUTED,align='center',action='monitor_open')

    def cards(p):
        for i in range(16):
            key='card_'+str(i)
            position = i
            x,y=12+(position%2)*230,156+(position//2)*140
            width,height=226,136
            inset,name_height,arrow_size=12,30,40
            name_y=height-inset-name_height
            card=group(p,key,x,y,width,height,True)
            box(card,0,0,226,height,CARD,12)
            toggle='slot_'+str(i)+'_toggle'
            action='slot_'+str(i)+'_open'
            btn(card,0,0,226,76,'',toggle,CARD,r=12)
            dynamic_art(card,'slot',key,'light','light-on',inset,inset,56,56,toggle)
            DYNAMIC_ICONS[-1].update({'slot':i})
            text(card,78,24,136,28,'',16,MUTED,key+'_state',align='right',action=toggle)
            on = group(card,key+'_state_on',0,0,226,65)
            off = group(card,key+'_state_off',0,0,226,65)
            objs.append(container(on,155,32,18,18,bg='#501F12',radius=9,
                                  name=P+key+'_glow',bind=P+key+'_glow'))
            objs.append(image(on,ASSETS+'state-on.svg',158,35,12,12))
            objs.append(image(off,ASSETS+'state-off.svg',149,35,12,12))
            text(on,178,24,37,32,'ON',24,ORANGE,action=toggle)
            text(off,169,24,46,32,'OFF',24,MUTED,action=toggle)
            btn(card,0,76,226,height-76,'',action,CARD,r=12)
            text(card,inset,name_y,160,name_height,'',24,binding=key+'_name',action=action)
            art(card,'chevron-right',width-inset-arrow_size,name_y+(name_height-arrow_size)//2,
                arrow_size,arrow_size,action)
            DEVICE_SLOTS.append({'slot':i,'visible_binding':P+key+'_visible',
                'name_binding':P+key+'_name','state_binding':P+key+'_state',
                'open_action':P+action,'toggle_action':P+toggle,
                'on_icon_binding':P+key+'_icon_on_visible','off_icon_binding':P+key+'_icon_off_visible'})

    home=group(parent,'home')
    box(home,0,0,480,480,BG,0)
    header(home,'Home',True)
    viewport=len(objs)
    objs.append(container(home,0,64,480,416,bg=BG,name=P+'home_viewport'))
    objs[viewport]['clip_children']=True
    content=group(viewport,'home_scroll',0,0,480,1676,False)
    objs[content]['y']={'default':0,'min':-1260,'max':0}
    both=group(content,'both',0,0,480,243)
    camera(both,3,'both')
    camera_only=group(content,'camera_only',0,0,480,400)
    camera(camera_only,3,'camera_only')
    text(camera_only,24,276,432,64,'Connect an Interaction Module\nto add device controls.',
         20,MUTED,align='center')
    devices=group(content,'home_devices',0,0,480,1424)
    objs[devices]['y']={'default':0,'min':0,'max':243}
    heading(devices,0,'interaction_only')
    automation(devices,64,'interaction_only')
    cards(devices)
    none=group(home,'none')
    art(none,'no-module-connected',183,113,113,113)
    text(none,12,242,456,44,'No Module Connected',32,align='center')
    text(none,12,295,456,34,'Connect a module to get started',23,'#B4B6BA',align='center')
    feedback(home,'home')

    for key,title,device in (('light','Light','Light Name'),):
        p=group(parent,key)
        box(p,0,0,480,480,BG,0)
        header(p,title)
        text(p,18,66,286,40,device,32,binding=key+'_name')
        if key=='light':
            text(p,278,82,78,32,'--',22,MUTED,'light_state',align='right',action='light_toggle')
            dynamic_iconbutton(p,'light_power','light_power','off','on',360,76,110,48,'light_toggle')
            colour_link=group(p,'colour_link',408,8,48,48,True)
            iconbutton(colour_link,'info',0,0,48,48,'colour_open')
            presets=group(p,'light_presets',0,0,480,480,True)
            for i,(preset,icon,title2) in enumerate((('night','moon-fall','Night'),('relax','relax','Relax'),('reading','book-open','Reading'),('gaming','gamepad','Gaming'))):
                x=18+i*125
                iconbutton(presets,icon,x+8,136,48,48,'light_preset_'+preset)
                text(presets,x,188,80,25,title2,18,'#D6D6DE',action='light_preset_'+preset)
            controls=group(p,'light_controls',7,220,466,252,True)
            panel=box(controls,0,0,466,252,'#2B2B2B',55)
            temperature=group(panel,'light_temperature',0,0,466,105,True)
            text(temperature,23,15,270,32,'Colour Temperature',24)
            text(temperature,295,17,148,28,'-- K',18,MUTED,'light_kelvin',align='right')
            track=box(temperature,23,54,418,24,'#00000000',0)
            objs.append({'type':'progress','parent':track,'x':0,'y':3,'w':418,'h':18,
                         'min':0,'max':418,'value':11,'bg_color':'#181819','fg_color':'#3B3C3D',
                         'radius':9,'name':P+'temperature_fill','bind':P+'temperature_fill'})
            for i,x in enumerate((9,76,152,260,407)):
                filename = 'temperature-mark-last.svg' if i == 4 else 'temperature-mark.svg'
                objs.append(image(track,ASSETS+filename,x-5,7,10,10))
                text(temperature,min(393,max(23,23+x-25)),80,50,25,
                     '',18,MUTED,'temperature_tick_'+str(i),align='center')
            thumb=group(track,'temperature_thumb',0,0,23,24,True)
            objs[thumb]['x']={'default':0,'min':0,'max':396}
            box(thumb,0,0,23,24,ORANGE,2)
            level=group(panel,'light_level',0,110,466,142,True)
            text(level,23,1,280,32,'Brightness',24)
            # Sparse ticks stay fixed while the filled stroke follows the thumb.
            art(level,'brightness-track-sparse',0,0,466,142)
            # Narrow, fixed stroke strips meet under the opaque circular thumb.
            # No moving clip or displaced image is needed on the panel renderer.
            for i,x,y,w,h in [(1, 17, 98, 15, 31), (2, 32, 87, 16, 41), (3, 48, 77, 16, 38), (4, 64, 68, 16, 36), (5, 80, 61, 16, 33), (6, 96, 54, 16, 32), (7, 112, 49, 16, 30), (8, 128, 45, 16, 29), (9, 144, 41, 16, 28), (10, 160, 38, 16, 27), (11, 176, 36, 16, 26), (12, 192, 35, 16, 25), (13, 208, 34, 16, 25), (14, 224, 33, 16, 25), (15, 240, 34, 16, 24), (16, 256, 34, 16, 26), (17, 272, 36, 16, 26), (18, 288, 38, 16, 26), (19, 304, 40, 16, 28), (20, 320, 44, 16, 29), (21, 336, 48, 16, 30), (22, 352, 53, 16, 32), (23, 368, 59, 16, 34), (24, 384, 67, 16, 35), (25, 400, 75, 16, 37), (26, 416, 85, 16, 40), (27, 432, 96, 16, 33), (28, 448, 110, 3, 5)]:
                band=group(level,'brightness_band_'+str(i),x,y,w,h,True)
                art(band,'brightness-fill-band-'+str(i),0,0,w,h)
            thumb=group(level,'brightness_thumb',2,23,46,46,True)
            objs[thumb]['x']={'default':2,'min':2,'max':419}
            objs[thumb]['y']={'default':23,'min':22,'max':96}
            objs.append(image(thumb,ASSETS+'brightness-knob.svg',0,0,46,46))
            iconbutton(level,'remove',106,91,40,40,'light_brightness_minus','#2B2B2B')
            text(level,151,80,136,61,'--%',46,WHITE,'light_brightness',align='center')
            iconbutton(level,'add',294,91,40,40,'light_brightness_plus','#2B2B2B')
        feedback(p,key)

    colour=group(parent,'colour')
    box(colour,0,0,480,480,BG,0)
    header(colour,'Colour',back='colour_back')
    text(colour,18,80,444,44,'',32,binding='colour_name')
    for axis,title,y in (('hue','Hue',144),('saturation','Saturation',292)):
        section=box(colour,12,y,456,132,'#2B2B2B',32)
        text(section,22,17,230,34,title,24)
        text(section,252,19,182,30,'--',20,MUTED,'colour_'+axis,align='right')
        btn(section,12,64,40,44,'-',axis+'_minus',CARD,size=24,r=12)
        objs.append({'type':'progress','parent':section,'x':64,'y':79,'w':328,'h':14,
                     'value':14,'min':0,'max':328,'name':P+axis+'_slider','bind':P+axis+'_slider',
                     'bg_color':'#181819','fg_color':ORANGE,'radius':7})
        thumb=group(section,axis+'_thumb',64,72,28,28,True)
        objs[thumb]['x']={'default':64,'min':64,'max':364}
        box(thumb,0,0,28,28,WHITE,14)
        btn(section,404,64,40,44,'+',axis+'_plus',CARD,size=24,r=12)
    feedback(colour,'colour')

    sensor=group(parent,'sensor')
    box(sensor,0,0,480,480,BG,0)
    header(sensor,'Sensor')
    text(sensor,18,80,444,44,'',32,binding='sensor_name')
    text(sensor,18,144,444,36,'',24,MUTED,'sensor_kind')
    value_panel=box(sensor,12,210,456,180,'#2B2B2B',36)
    text(value_panel,24,58,408,64,'',40,binding='sensor_value',align='center')
    feedback(sensor,'sensor')

    monitor=group(parent,'monitor')
    box(monitor,0,0,480,480,BG,0)
    art(monitor,'monitor_camera_canvas',0,0,480,480,binding='monitor_camera_canvas')
    objs[-1]['hidden']=True
    clear='#00000000'
    shade='#00000066'
    divider='#3B3C3D'
    secondary='#B4B6BA'
    nofeed=group(monitor,'monitor_camera_unavailable',hidden=False)
    art(nofeed,'hm-monitoring-off',176,160,127,100)
    text(nofeed,12,274,456,48,'Camera Unavailable',32,align='center')
    text(nofeed,12,326,456,28,'',18,MUTED,'monitor_state',align='center')
    off=group(monitor,'monitor_camera_off')
    box(off,0,0,480,480,'#1A1A1A',0)
    art(off,'hm-monitoring-off',176,160,127,100)
    text(off,12,274,456,48,'Monitoring Off',36,align='center')
    fail=group(monitor,'monitor_privacy_failure')
    box(fail,0,0,480,480,BG,0)
    text(fail,12,174,456,46,'Privacy processing unavailable',28,align='center')
    text(fail,12,223,456,30,'Live view hidden for your privacy',18,secondary,align='center')

    controls=group(monitor,'monitor_controls',12,404,456,64,False)
    for enabled in (False,True):
        power=group(controls,'monitor_power_'+('on' if enabled else 'off'),0,0,64,64,enabled)
        btn(power,0,0,64,64,'','monitor_toggle',ORANGE if enabled else shade,r=32)
        inset=14 if enabled else 8
        size=64-2*inset
        art(power,'hm-power-on' if enabled else 'hm-control-power-off',inset,inset,size,size,'monitor_toggle')
    box(controls,93,0,270,64,shade,32)
    for x,mode,title in ((97,'normal','Normal'),(230,'privacy','Privacy')):
        action='monitor_'+mode
        btn(controls,x,4,129,56,'',action,clear,r=28)
        text(controls,x,4,129,56,title,24,'#D6D6DE',align='center',action=action)
        selected=group(controls,action+'_selected',x,4,129,56,mode!='normal')
        btn(selected,0,0,129,56,'',action,'#FFFFFF',r=28)
        text(selected,0,0,129,56,title,24,'#1A1A1A',align='center',action=action)
    btn(controls,392,0,64,64,'','menu_open',shade,r=32)
    art(controls,'hm-control-more',400,8,48,48,'menu_open')

    privacy_modes=(('all_faces','All Faces','all-faces'),
                   ('select_faces','Select Faces','select-faces'),
                   ('background','BG Blur','background-blur'))
    tray=group(monitor,'monitor_privacy_tray')
    btn(tray,0,64,480,300,'','privacy_open',clear,r=0)
    box(tray,54,364,372,104,shade,52)
    for (key,title,asset),(x,width) in zip(privacy_modes,((78,88),(190,96),(310,88))):
        action='privacy_'+key
        btn(tray,x,372,width,88,'',action,clear,r=0)
        art(tray,'hm-mode-'+asset,x+(width-48)//2,380,48,48,action)
        text(tray,x-12,435,width+24,24,title,18,align='center',action=action)
        selected=group(tray,'monitor_tray_'+key+'_selected',x-12,372,width+24,88,key!='all_faces')
        art(selected,'hm-mode-'+asset+'-on',12+(width-48)//2,8,48,48,action)
        text(selected,0,63,width+24,24,title,18,ORANGE,align='center',action=action)

    iconbutton(monitor,'hm-live-back',24,8,48,48,'home_open',clear)
    text(monitor,80,15,232,34,'',24,'#FFFFFF','monitor_timestamp')
    for key,_,asset in privacy_modes:
        mode=group(monitor,'monitor_privacy_mode_'+key,352,12,40,40)
        iconbutton(mode,'hm-mode-'+asset+'-on',0,0,40,40,'privacy_open',clear)
    iconbutton(monitor,'hm-live-info',416,12,40,40,'monitor_info',clear)
    feedback(monitor,'monitor',332)

    menu=group(parent,'menu')
    box(menu,0,0,480,480,BG,0)
    header(menu,'Monitor',back='monitor_open')
    for i,(key,title,icon,action) in enumerate((('live','Live View','hm-live','monitor_live'),
                                                ('family','Family','hm-family','family_open'),
                                                ('activity','Activity Log','hm-activity','activity_open'))):
        y=64+i*100
        btn(menu,0,y,480,99,'',action,BG,r=0)
        art(menu,icon,12,y+26,48,48,action)
        text(menu,72,y+27,350,46,title,32,action=action)
        selected=group(menu,'menu_'+key+'_selected',72,y,396,99,key!='live')
        box(selected,0,24,350,50,BG,0,action)
        text(selected,0,27,350,46,title,32,ORANGE,action=action)
        art(selected,'hm-check',356,30,40,40,action)
        box(menu,0,y+99,480,1,divider,0)

    family=group(parent,'family')
    box(family,0,0,480,480,BG,0)
    header(family,'Family',back='menu_open')
    iconbutton(family,'hm-plus',416,12,40,40,'family_add',clear)
    empty=group(family,'family_empty',hidden=False)
    art(empty,'hm-empty-people',194,110,92,59)
    text(empty,12,182,456,46,'No Members Yet',36,align='center')
    text(empty,12,229,456,34,'Add a person to get started.',24,secondary,align='center')
    btn(empty,64,368,352,88,'Add Member','family_add',ORANGE,32,44)
    members=group(family,'family_list',0,64,480,398)
    for i in range(3):
        row=group(members,f'family_row_{i}',0,i*100,480,100)
        box(row,12,26,48,48,divider,8)
        text(row,72,27,260,46,'',32,binding=f'family_name_{i}')
        objs[-1]['overflow']='clip'
        btn(row,356,0,124,99,'Delete',f'family_delete_{i}','#D00000',24,0)
        box(row,0,99,480,1,divider,0)
    text(members,12,375,456,30,'Members: 0/3',18,binding='family_count',align='center')
    feedback(family,'family')

    member=group(parent,'family_member')
    box(member,0,0,480,480,BG,0)
    header(member,'Add Family Member',back='family_member_back')
    art(member,'hm-check-disabled',416,12,40,40)
    save=group(member,'family_member_save_enabled',416,12,40,40)
    iconbutton(save,'hm-check',0,0,40,40,'family_member_save',BG)
    text(member,12,75,456,43,'Add a family member by name',28,align='center')
    art(member,'hm-face-capture',130,135,220,220)
    text(member,12,353,456,29,'Face recognition unavailable',18,secondary,align='center')
    btn(member,53,389,390,49,'','family_member_name',BG,r=0)
    text(member,53,389,390,49,'Enter Member Name',24,MUTED,'family_member_name_preview',action='family_member_name')
    objs[-1]['overflow']='clip'
    box(member,53,437,390,2,ORANGE,0)
    keyboard=group(member,'family_member_keyboard',0,64,480,416)
    objs[keyboard]['name']=P+'family_member_keyboard_sheet'
    box(keyboard,0,0,480,416,BG,0)
    text(keyboard,45,39,390,49,'',24,WHITE,'family_member_draft')
    objs[-1]['overflow']='clip'
    box(keyboard,45,87,390,2,ORANGE,0)
    box(keyboard,0,151,480,265,CARD,0)
    objs.append({
        'type':'keyboard', 'parent':keyboard, 'x':0, 'y':167, 'w':480, 'h':233,
        'name':P+'family_member_keyboard', 'bg_color':CARD, 'fg_color':'#ECECF0',
        'font_size':24, 'key_color':'#55555A', 'function_color':divider,
        'function_text_color':'#ECECF0', 'delete_color':divider,
        'delete_text_color':'#ECECF0', 'space_color':'#55555A',
        'space_text_color':'#ECECF0', 'ok_color':ORANGE, 'ok_text_color':WHITE,
        'key_radius':8, 'shift_label':'SHIFT', 'delete_label':'DELETE',
        'ok_label':'GO', 'symbols_label':'ABC', 'letters_label':'ABC',
        'space_label':'', 'function_font_size':20,
        'shift_icon':ASSETS+'hm-keyboard-shift.png', 'delete_icon':ASSETS+'hm-keyboard-delete.png',
    })
    feedback(member,'family_member',185)

    faces=group(parent,'privacy_faces')
    box(faces,0,0,480,480,BG,0)
    screen=group(faces,'privacy_faces_screen',hidden=False)
    text(screen,12,169,456,46,'Face detection unavailable',28,align='center')
    text(screen,12,217,456,31,'Live view hidden for your privacy',18,secondary,align='center')
    text(screen,24,8,132,42,'On Screen',24,action='privacy_faces_screen')
    box(screen,80,43,13,13,ORANGE,0)
    text(screen,163,8,165,42,'Saved People',24,MUTED,action='privacy_faces_saved')
    box(screen,64,368,352,88,'#892C07',44)
    text(screen,64,368,352,88,'Done',32,MUTED,align='center')
    saved=group(faces,'privacy_faces_saved')
    art(saved,'hm-empty-people',194,110,92,59)
    text(saved,12,182,456,46,'No Saved Faces',36,align='center')
    text(saved,12,228,456,34,'Face recognition unavailable',24,secondary,align='center')
    text(saved,24,8,132,42,'On Screen',24,MUTED,action='privacy_faces_screen')
    text(saved,163,8,165,42,'Saved People',24,action='privacy_faces_saved')
    box(saved,239,43,13,13,ORANGE,0)
    btn(saved,64,368,352,88,'Add Member','family_add',ORANGE,32,44)
    iconbutton(faces,'back',408,8,48,48,'privacy_faces_back',clear)
    feedback(faces,'privacy_faces',332)

    activity=group(parent,'activity')
    box(activity,0,0,480,480,BG,0)
    header(activity,'Activity Log',back='menu_open')
    art(activity,'hm-activity',12,88,48,48)
    text(activity,72,82,396,40,'No Activity Yet',32)
    box(activity,0,163,480,1,divider,0)
    feedback(activity,'activity')

    return home
