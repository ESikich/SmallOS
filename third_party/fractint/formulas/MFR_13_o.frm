multifractal_13 { ;  Albrecht Niekamp  Jan06,2015 Upd Warpeffect
;P1      Bees-Julia-Manowar-(Lambda)-(Magnet) seed
;RP2 Left: 5 Digit_Channels: (1)shape (2)out (3)ins_1 (4)ins_2 (5)ins_3
;     Frm: 0_off 1_Secant 2_Mand(2) 3_Bees 4_Jul/Manowar 5_Mmods
;          6_Phoenix 7_Newton 8_Lambda/Magnet
;   Right: 2 Digit_ManyMods_Number of sides  2 Digit_Phoen  2 Digit_Lambd
;          1 Digit_TransReset-Shape: 0_no 1_DblMan 2_Iter 3_both +5_nowarp
;          4 Digit_TransReset-Ch 2-5: 0_no 1_z 2_Iter 3_both +5_warp
;IP2 Left: 5 Digit_Bailout Number for Channels 1 to 5
;   Right: Variables: 4 Digit_Mand/Jul (2var)  2 Digit_Secant 4 Digit_Bees
;RP3 Left: 2 Digit_Newtonvariable  4 Digits_bailout1
;   Right: 4 Digit_bailout2 4 Digit_bailout3 1 Digit_Magnet 
;IP3 Left: 4 Digit_Shape:  Warp-factor (fn1 is used)
;   Right: 4 Digit_bailout4 4 Digit_bailout5 1 digit_mow=2 1 digit_mag=2
;RP4 Left: 4 Digit_Outside:  Warp-factor (fn2 used)
;   Right: Outside: 4 Digit+fractdig_Border-out 4 Digit+fractdig_border-in
;IP4 Left: Inside1_Maxiter
;   Right: Inside1_Transit: 1_maxit 2_borderout 3_borderin +5_maxit+bord
;          5 Digit_warp factor (fn2 used)  4 Digit+fractaldigit_border1
;RP5 Left: Inside2_Maxiter
;   Right: Inside2_Transit: 1_maxit 2_borderout 3_borderin +5_maxit+bord
;          5 Digit_warp factor (fn3 used)  4 Digit+fractaldigit_border2
;IP5 Left: Inside3_Maxiter
;   Right: Inside3_Transit: 1_maxit 2_borderout 3_borderin +5_maxit+bord
;          5 Digits_Warp factor (fn4 used)  4 Digit+fractaldigit_border3
;fn(1) shared by many-mods + bees
;
z=pixel
da=real(p2)
dd=trunc(da)
tt=dd>0
da=trunc(((da-dd)*100000000000)+11111)
dd=trunc(dd+11111)
d=trunc(dd/10000)
dd=dd-d*10000
d3=(d==4)+(d==5)+(d==8)+(d==9)
d4=d3==0
vb=d>5
ex0=(d>1)
sc=d==2
mo=d==6
po=d==7
sx=d==9
ab=po+(d==3)+(d==5)+(d==9)
d=trunc(dd/1000)
dd=dd-d*1000
ex1=(d>1)
sc1=d==2
mo1=d==6
po1=d==7
sx1=d==9
v1m=mo1+po1
v1j=d>7
dd1=v1j+(d==4)+(d==5)
ab1=po1+(d==3)+(d==5)+(d==9)
d=trunc(dd/100)
dd=dd-d*100
ex2=(d>1)
sc2=d==2
mo2=d==6
po2=d==7
sx2=d==9
v2m=mo2+po2
v2j=d>7
dd2=v2j+(d==4)+(d==5)
ab2=po2+(d==3)+(d==5)+(d==9)
d=trunc(dd/10)
ex3=(d>1)
sc3=d==2
mo3=d==6
po3=d==7
sx3=d==9
v3m=mo3+po3
v3j=d>7
dd3=v3j+(d==4)+(d==5)
ab3=po3+(d==3)+(d==5)+(d==9)
d=dd-d*10
ex4=(dd==2)
sc4=d==2
mo4=d==6
po4=d==7
sx4=d==9
v4m=mo4+po4
v4j=d>7
dd4=v4j+(d==4)+(d==5)
ab4=po4+(d==3)+(d==5)+(d==9)
tt=ex1+ex0+ex2+ex3+ex4
;
mm=trunc(da/1000000000)
da=da-mm*1000000000
ph=trunc(da/10000000)
da=da-ph*10000000
sp=trunc(da/100000)
da=da-sp*100000
d=trunc(da/10000)
da=da-d*10000
wx=(d>4)
d=d-5*wx
dm=(d==2)+(d==4)
ir0=(d==3)+(d==4)
d=trunc(da/1000)
da=da-d*1000
w1=d>4
d=d-5*w1
rs1=(d==2)+(d==4)
ir1=(d==3)+(d==4)
d=trunc(da/100)
da=da-d*100
w2=d>4
d=d-5*w2
rs2=(d==2)+(d==4)
ir2=(d==3)+(d==4)
d=trunc(da/10)
da=da-d*10
w3=d>4
d=d-5*w3
rs3=(d==2)+(d==4)
ir3=(d==3)+(d==4)
w4=da>4
da=da-5*w4
rs4=(da==2)+(da==3)+(da==4)
ph=ph/10
w0=(wx==0)
If (1<sp)
d=(trunc(sp/10))/9
da=fn4(sp-100*d)
d=sp/73,37
sp=d+sqr(-da)
else
sp=p1
endif
;
d=real(p3)
dd=trunc(d)
da=trunc((d-dd)*10000000000)
pp=trunc(dd/10000)
ba1=dd-10000*pp
ba2=trunc(da/1000000)
da=da-1000000*ba2
ba3=trunc(da/100)
da=da-100*ba3-4
mg=da/10
if (mg<1)
mg=real(p1)
endif
;
d=imag(p3)
dd=trunc(d)
da=trunc((d-dd)*10000000000)
sfac=dd/1000
ba4=trunc(da/1000000)
da=da-ba4*1000000
ba5=trunc(da/100)
da=da-ba5*100
d=trunc(da/10)
da=da-d*10
mow=d==2
mag=da==2
px0=((sx*mag)==0)*p1
px1=((sx1*mag)==0)*p1
px2=((sx2*mag)==0)*p1
px3=((sx3*mag)==0)*p1
px4=((sx4*mag)==0)*p1
;
d=real(p4)
dd=trunc(d)
da=trunc((d-dd)*10000000000)
ofac=dd/1000
bh=trunc(da/100000)/10
bl=(da-bh*1000000)/10
bs=bl/2
;
d=imag(p2)
dd=trunc(d)
da=trunc((d-dd)*10000000000)
d=trunc(dd/10000)
dd=dd-d*10000
bb0=ba1*(d==1)+ba2*(d==2)+ba3*(d==3)+ba4*(d==4)+ba5*(d==5)
d=trunc(dd/1000)
dd=dd-d*1000
bb1=ba1*(d==1)+ba2*(d==2)+ba3*(d==3)+ba4*(d==4)+ba5*(d==5)
d=trunc(dd/100)
dd=dd-d*100
bb2=ba1*(d==1)+ba2*(d==2)+ba3*(d==3)+ba4*(d==4)+ba5*(d==5)
d=trunc(dd/10)
dd=dd-d*10
bb3=ba1*(d==1)+ba2*(d==2)+ba3*(d==3)+ba4*(d==4)+ba5*(d==5)
d=dd
bb4=ba1*(d==1)+ba2*(d==2)+ba3*(d==3)+ba4*(d==4)+ba5*(d==5)
;
d=da
p0=trunc(d/100000000)/10
d=d-p0*1000000000
p6=trunc(d/1000000)/10
d=d-p6*10000000
p7=trunc(d/10000)/10
d=d-p7*100000
dp=p6+p0/100
p8=trunc(d/100)/100
d=d-p8*10000
p9=d/100
;
d=imag(p4)
mi1=trunc(d)
da=trunc((d-mi1)*100000000000)
d=trunc(da/10000000000)
bt1=d>6
da=da-d*10000000000
d=d-5*bt1
dt1=d>1
iv1=d==3
dd=trunc(da/100000)
da=da-dd*100000
fac1=dd/1000
bo1=(da/100000)/10
;
d=real(p5)
mi2=trunc(d)
da=trunc((d-mi2)*100000000000)
d=trunc(da/10000000000)
bt2=d>6
da=da-d*10000000000
d=d-5*bt2
dt2=d>1
iv2=d==3
dd=trunc(da/100000)
da=da-dd*100000
fac2=dd/1000
bo2=(da/100000)/10
;
d=imag(p5)
mi3=trunc(d)
da=trunc((d-mi3)*100000000000)
d=trunc(da/10000000000)
bt3=d>6
da=da-d*10000000000
d=d-5*bt3
dt3=d>1
iv3=(d==3)
dd=trunc(da/100000)
da=da-dd*100000
fac3=dd/1000
bo3=(da/100000)/10
;
if (vb)
 if (d3)
  if (ab)
   if (mag)
    c=z
    z=pixel
    x=mg
    x=x+(x==0)*3            ;magnet
   else
    z=pixel                 ;lambdafn
    x = ((1, 0.4) * (|sp|<=0) + sp)
    m = (64 * (real(ba1)<=0) + real(ba1) * (0<ba1) )
   endif
  else
   c=z
   z=pixel                 ;newton
  endif
 elseif (ab)
   c=z                     ;Phoenix
   z=pixel
  else
   c=0.4*log(sqr(z^mm))     ;many mods
   z=0
 endif
elseif (d3)
 if (ab)
   if (mow)                  ;manowar
     mt = (4 * (real(p2)<=0) + real(p2) * (0<p2) )
     c=p1
     z=pixel
   else
    c=p1                      ;Julia
    z=pixel
   endif
 else                         ;bees
  c=p1
  z=pixel
 endif
elseif (ab)
 c=z                          ;Mandel
 z=0
else
 c=z                           ;Secant
 z=pixel
endif
t=0
bo=|z|
p=pp
z0=p7
zold=(0.0,0.0)
cb=p9
ba=bb0
:
if (tt>0)
 t=t+1
 if (ex0)
  ex0=t<mi1
  if (bo>bs)
   u0=fn1(t*sfac)
   if (w0)
    u=u0
   else
    u=1.0
   endif
   ex0=0
   if (ir0)
    t=0
   endif
    if (d4)
    z=z*u
    if (mo)
     c=0.4*log(sqr(z^mm))
    else
     c=pixel
    endif
   else
    z=pixel
    cb=p9*u
    c=px0*u
    p=pp*u
    x=x*u
   endif
   tt=tt-1+ex0
  endif    
 elseif ((ex1)&&bo>bl)
  if (bo<bh)
   d3=dd1
   ba=bb1
   ab=ab1
   ex1=0
   tt=tt-1
   if (w1)
    u=fn2(t*ofac)
   else
    u=1.0
   endif
   if (ir1)
    t=0
   endif
   if (d3)
    vb=v1j
   if (rs1)
     z=pixel
     cb=p9*u
     c=px1*u
     p=pp*u
     x=x*u
    else
     c=px1
     z=z*u
     cb=p9
    endif
   else
    vb=v1m
    if (rs1)
     c=z*u
     z=pixel*(sc1+po1)
     z0=p7*u
     ph=ph*u
    else
     c=z
     z=z*u
    endif
    if (mo1)
     c=0.4*log(sqr(z^mm))
    endif
   endif
  endif
 elseif (ex2)
  if (dt1)
   if (iv1)
    d=bo>bo1
   else
    d=bo<bo1
   endif
   if (bt1)
    d=d+(t>mi1)
   endif
  else
   d=t>mi1
  endif
  if (d)
   ab=ab2
   d3=dd2
   ba=bb2
   ex2=0
   tt=tt-1
   if (w2)
    u=fn2(t*fac1)
   else
    u=1.0
   endif
   if (ir2)
    t=0
   endif
   if (d3)
    vb=v2j
    if (rs2)
     z=pixel
     cb=p9*u
     c=px2*u
     p=pp*u
     x=x*u
    else
     cb=p9
     c=px2
     z=z*u
    endif
   else
    vb=v2m
    if (rs2)
     c=z*u
     z=pixel*(sc2+po2)
     z0=p7*u
     ph=ph*u
    else
     c=z
     z=z*u
    endif
    if (mo2)
     c=0.4*log(sqr(z^mm))
    endif
   endif
  endif
 elseif (ex3)
  if (dt2)
   if (iv2)
    d=bo>bo2
   else
    d=bo<bo2
   endif
   if (bt2)
    d=d+(t>mi2)
   endif
  else
   d=t>mi2
  endif
  if (d)
   ab=ab3
   d3=dd3
   ba=bb3
   ex3=0
   tt=tt-1
   if (w3)
    u=fn3(t*fac2)
   else
    u=1.0
   endif
   if (ir3)
    t=0
   endif
   if (d3)
    vb=v3j
    if (rs3)
     z=pixel
     cb=p9*u
     c=px3*u
     p=pp*u
     x=x*u
    else
     cb=p9
     c=px3
     z=z*u
    endif
   else
    vb=v3m
    if (rs3)
     c=z*u
     z=pixel*(sc3+po3)
     z0=p7*u
     ph=ph*u
    else
     c=z
     z=z*u
    endif
    vb=v3m
    if (mo3)
     c=0.4*log(sqr(z^mm))
    endif
   endif
  endif
 elseif (ex4)
  if (dt3)
   if (iv3)
    d=bo>bo3
   else
    d=bo<bo3
   endif
   if (bt3)
    d=d+(t>mi3)
   endif
  else
   d=t>mi3
  endif
  if (d)
   ab=ab4
   d3=dd4
   ba=bb4
   ex4=0
   tt=0
   if (w4)
    u=fn4(t*fac3)
   else
    u=1.0
   endif
   if (d3)
    vb=v4j
    if (rs4)
     z=pixel
     cb=p9*u
     c=px4*u
     p=pp*u
     x=x*u
    else
     cb=p9
     c=px4
     z=z*u
    endif
   else
    vb=v4m
    if (rs4)
     c=z*u
     z=pixel*(sc4+po4)
     z0=p7*u
     ph=ph*u
    else
     c=z
     z=z*u
    endif
    if (mo4)
     c=0.4*log(sqr(z^mm))
    endif
   endif
  endif
 endif
endif
if (vb)
 if (d3)
  if (ab)
   if (mag)                      ;magnet
    z=((z^x+c-1)/(2*z+c-2))^(x-1)
   else
    z=fn2(z)*fn3(x)              ;Lambdafn
    abs(imag(z)) <=x && abs(real(z)) <=m
   endif
  else
   z1=z^p-1                      ;newton
   z2=p*z*z
   z=z-z1/z2
  endif
 elseif (ab)
   z1=z*z+0.56+ph/100-0.5*zold  ;Phoenix
   zold=z
   z=z1
 else
  z2=fn1(z)+c                   ;Many_mods
  z1=cos(z2)
  z=c*(1-z1)/(1+z1)
 endif
elseif (d3)
 if (ab)
  if (mow)
   z1 = z                       ;manowar
   oldz = z
   z = sqr(oldz) + z1 + c
   z1 = oldz
  else
   z2=z*z                       ;Julia
   z=z2*z2+p6*z2+c-p0
  endif
 else
  z1=fn1(z)-cb                  ;Bees
  z2=z1^p8-1
  z3=p8*(z1^(p8-1))
  z=z-(z2/z3)
 endif
elseif (ab)
 if (dm)
   z=z*z+c+c*c-dp               ;Double Mandel
 else
  z2=z*z                        ;Mandel
  z=z2*z2+p6*z2+c-p0
 endif
else
 z3=z                           ;Secant
 z1=z0*z0*z0*z0-1
 z2=z*z*z*z-1
 z=z-z2*(z-z0)/(z2-z1)
 z0=z3
endif
bo=|z|
bo<ba
}

lambdafn {; Chuck Ebbert.
          ; P1 is lambda, P2 = bailout (default 64).
          ; lambda default is (1,.4)
  z = pixel
   ; CAE added leading zero below in (1,.4)   12 Jan 1993
  m = ((1, 0.4) * (|p1|<=0) + p1 )   ; force (1,.4) when p1=0
  t = (64 * (real(p2)<=0) + real(p2) * (0<p2) ):
  z = fn1(z) * m
  abs(imag(z)) <= t && abs(real(z)) <= t
  ;SOURCE: builtn2.frm
}


LambdaFn (ORIGIN) {; v7.x/9.1 - Chuck Ebbert - from BUILTN.FRM
   ; p1 is lambda (default = 1.0,0.4), 
   ; real(p2) changes bailout (default = 64).
   ; float=yes, set co-ords = -4/3/4/-3              - JH
  z = pixel
   ; CAE added leading zero below in (1,.4)                 12 Jan 1993
  m = ((1, 0.4) * (|p1|<=0) + p1 )            ; force (1,0.4) when p1=0
  t = (64 * (real(p2)<=0) + real(p2) * (0<p2) ):
  z = fn1(z) * m
  abs(imag(z)) <= t && abs(real(z)) <= t
  ;SOURCE: parser.frm
}


lambdafn { 
  z = pixel
  m = ((1, 0.4) * (|p1|<=0) + p1 )  
  t = (64 * (real(p2)<=0) + real(p2) * (0<p2) ):
  z = fn1(z) * fn2(m)
  abs(imag(z)) <= t && abs(real(z)) <= t
  ;SOURCE: bejones.frm
}