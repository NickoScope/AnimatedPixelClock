-- @upload-only
-- @by openclaw
-- SOTD 0923 EVENING - Screen of the Day for 23 September 2026, the evening one, second take: a waterfront at dus
-- EVENING WATERFRONT: indigo, violet and gold above a city becoming stars.
-- The stable entropy field is not padding: every byte feeds architecture, windows,
-- stars, clouds, water glints and their permanent phases. Nothing is reseeded.
FPS=18
PERIOD=240
local floor,sin,cos,abs,min,max=math.floor,math.sin,math.cos,math.abs,math.min,math.max
local tau=math.pi*2
local W,H=px.size()
local _C,_L,_R,_P,_T,_B,_G,_Q=px.circle,px.line,px.rect,px.pixel,px.text,px.blend,px.glow,px.get
local function Circle(x,y,r,a,b,c,f)_C(floor(x),floor(y),floor(r),floor(a),floor(b),floor(c),f)end
local function Line(x,y,u,v,a,b,c)_L(floor(x),floor(y),floor(u),floor(v),floor(a),floor(b),floor(c))end
local function Rect(x,y,w,h,a,b,c,f)_R(floor(x),floor(y),floor(w),floor(h),floor(a),floor(b),floor(c),f)end
local function Pixel(x,y,a,b,c)_P(floor(x),floor(y),floor(a),floor(b),floor(c))end
local function Text(x,y,s,a,b,c)_T(floor(x),floor(y),s,floor(a),floor(b),floor(c))end
local function Blend(x,y,a,b,c,d)_B(floor(x),floor(y),floor(a),floor(b),floor(c),d)end
local function Glow(x,y,r,a,b,c,d)_G(floor(x),floor(y),floor(r),floor(a),floor(b),floor(c),d)end
local function Get(x,y)return _Q(floor(x),floor(y))end
local function clamp(x,a,b)if x<a then return a elseif x>b then return b end return x end
local function mix(a,b,t)return floor(a+(b-a)*t+.5)end
local function smooth(a,b,x)local q=clamp((x-a)/(b-a),0,1)return q*q*(3-2*q)end
local FIELD=table.concat({
 "9!qSif8h)h9QHw@lBB9MA7h-B7kGc>p%&sL,ekOk+\\71k-\\@f'vRq`!p6#Jw5Y<1HJTW66VcP>CR_,z)wDdzy)*PR,T\"[^iR<cNC-z%)HF[z]P[u\\<LtwDuxq!UW=<zE'q1Tj1)n'XCK#:$2EI+.#A",
 "B2cJrDx_IN'u$wqi@n&_fwn:$S;VdVd*Ub`lVVs>MYpY.TyDfJ7OA+:tl_7?c9Y<U'.QyGKB2*a##7nNsW=85rPf8p].VQ&3Xjif9SCVF09!\\.u(Jx'W;H$6jtjr0PtmDcIq_r]Slr:&.EIy,*EebG",
 "6F58q^QhO<?=huoY<9hAA)3qTUR8AiD`61jZ/jK\"k=b-bX?r;G4hlXS!c;Y`P't-WX;RV,(v#LFT19]70O=<o0:$hvLAHy`fGrGg%EKO6!=gD[;%g*EN7J/47k2-G?/?6`0G0HKsk[`Xvwu6=(I^.l",
 ")?Y[T\"!'>+IS?Km.%T*weJ\"\\jhYJ,Qh\\K=V_$Vas@dK@>'K%N/FMGEssNEAt$=$f9]#i0x.[*?u%roSDH=?X*b\\_u6V0;IC1fk7U^5;7n@'CryEa)TDoK$ce'jnm:f]jJ14VMq+w;fOhDQ)XkqB8n6",
 "/Pv\\0tYo-Exr-E%p*>Q6%bHkY<X6=%K$-p^+Y9R'.d5lDW\"]U*b!`&&ZphQfco0gHPE27FoN>cCc8vmq'n#TX7IDgLnGNb5-MYXz)5<HlES=$K?xl_@:Q9@\"d.,XeV/cAi_Ke)/lt^-v,\\/8\\MqWZI",
 "\"1e/(ACft1THt#sugJA,:+Q].oZ&TJKCWgH#QLHTw(9iuu]ZU($k3)y+U'8P(4+dJ&Az:yqepfaEdD!k=-5+42:J7abOoV`9qGNJfaFSt81uVGF;re8+?(Y3GgMJFLhk`41jt(])R9\\KsRa\"#YvXy0",
 "JrK\"LEv5_4,Pjfvc<!%r\"b1mx2z3AF(iD9hG4<F)'Rbt%d2DaGVM@ijN0K(=*3T\"B:NCB!o68-6.]/-<Wa[kJxwHY5PMH31z-U34W%D3e=bV*uBx5RWy+;+N9n']]`4R'G^!,EF$T#rm?<JL.;PAto",
 "820SGQ[K\\6,,YZ(nn,S8t+0a,`c<Ffqvb$a6?J*<Yc6!`)k;juaT-0'-w0V>+bgs'C62?Wgk_&;6:$\"ECx7\\VSq[y_$\"jGobdgF6K2'1te[`@\\4Z%VAdSdDj\\VHb9QV2ku[&N._tYM-=1GYE*.%bQE",
 "2=bl;6lRW(kuSKZhX:7U%6,f*r+h+DmAV\"/JfGhVL]nebk+)'wAC@G\\Pq@Ep$sXwL>m0Z_KJgg6z*e]xCqKNF5u<KTrGm9s/ESc=zQOlf<hW7X/eU\"a,dvbgmd;Y`zDxRE;(6EB#3A\\o2@i1Q+UdiK",
 "&.jTA'$]X*VRXja@@2-7RlJ,2;-6N1R$TlxH)pq`ODHd$:'5cRzV96_<>0)Q$@Z-oYs!1mT3xXdwpqVAqv8d)AAz5(mIuKlD76ApFi-v`qp/h=Te[\\bmZ5F]ftCbj,iui7m.#0@iW\\1Xa84^;ikcY)",
 "F8$wCFiKE@e3rVeiLAwrD)kNTu9\"WMa2Sg$bCXnkO%VIhl*X^!^mbE.pF9,B:Xv6He*Fi>gH44=LeeLdxtdV(A?2(5oZs;*JeMA0Cv\\_p,_D?/>;.gKUWM:Y6945c89<PI)53U2C>q!rKJ%>Xo$KeY",
 "U:A1!2gi1$u<eEtkCN`?V'3o((1RBs;B2&B\"b)W=h<l\"jSos%1>'jAB5uNCKaWN+%J^9f[&#\\jT+eIdSO\\QKnA>/0\"xv-lFwFaHRDSw0iEI>\\pyh?oUus2UKA_X,wXo`PPl,fcpxtA^>W1;s.tzaVC",
 "[#^i.xx2h/>!A,#=Z%!\\Ze/a$4J07;1ahv*Suwu<9`mYSU]QwF42BrGQg,px_kwqbKzP9I0^Q]jzFQeY0s'4'=JbL](m#JQ'.L7z<e4*E[F+fc,wrAD.4VGSQ[<Y:@EDPQU\"xiN@+9q3>%:NEycZ+6",
 "I9.WiQ$q6h7Y%SeJJOKm`VeC\\q.$\\A_qHo\"e_fF!_y5;Oc_hHN`K*$S.7Z\\.%8GZ@d#`odK8_1xP3A@iUH$'9HPgi1(;x)ZZ*?[xszIBZGAZ'l.e\"=D/`J\\1EQ^TaDpkTcmDK-%n)(mK>vPFanh$%]",
 "f;*Q;Gmb3(\\HdzRIBmq0-!_z4_UJhvFMQm^GfNsw<bIgKC\\R\"GA!(>lphhics?u8^xT]uJ3x4OSq)P2GbHPmt0j#M^UY1%\"nO46p2drcr3Q..SYo]dCT-iQT:#5zgv:`T=36#0NWx6O)9rv&1X=cA%",
 "c*dm$2HU7!Q^*&Xhq.r&Rx6N[Kp$v,\\,Q\"5b&1!A7a1SZI!4GUj!#4evl5?<U<N;0E)G`cq`P*o<p*0dq@cJEr>4v!_nXSmt\">JT25enhC^@#2O-+2>Vq&7D3$FHisI1bs6d5D<wAg5bB.UaKK/l*+",
 "eSO6%\\Yq23_)m<x4v09'=7bs_b@kQOvI,,M$*rZV5RHh%BsMehzA-B\\?-BU*ND]O%_GgNm;>=d&hwI)vJ)[!JupaGgb?\\;/exGO,098Fzw<po,gkABf`)sbv2N]\"DNy4j8Q'8g\"Te!76)GM8:sc8T7",
 "/^*!@DmX\"'@cVj5'P6Cj?=.0!NE]1q9=#[FW4@FZj,Ic<uU_mwWmeTO;940ZCQ>kL]vBtTKN5Cjs!%Xk?Wyv8jS0(zmG[[=CJhz6HV\"-^97Z6'8j/8,[1PY,\"'&FOGN^NEWx`/cCy:%w-!TBEP?h.G",
 "B8r'ijNMjd([vZpy\"=#fA'I;SR9g;D\"MvyUIw[>FwV3-Wy\\N2M*GI/I1YNa)n9lb'7$;[aPy:8ju8!ufxjox9N`h&A)0!f_^%U32GWFFA%HxjQWvK8dLW->!ejcGwg9[6UwPz1+JdmhAHZ)Q=Wx&hX",
 "`O%s>Ie\"!6>F8!jKk^W(SDeHusBnT,[W_GZ(H0(\"JixZ$?W$sSE]g5L1LKoBkMYaQyOU/1xabx$zG]/_@\"&Ah<T\\Lr.]+$D4;QYc>VJ3Pn\\GyXaJ\"p4P.nnjeP\\Oc)KP?AO(xR.UU\\&@C;-M0#Jve=",
 ",=)-[au\\mb9H;=@!^)kf$p1kZ,<r!knA]T\"uERy`w(wpp.O$iZj_=L_?y!U=[`FS8^]tdg\\*mN1$fgo[1$vq0PQ[4TP/m&cZ.(g+zxd>?Bw;qpb7Rj5Uv0Nx_4dY<tvx8%WMENYmgOaz/mHy[[7XKU",
 ".teSiZC<)St3OeF7i-\"t,&^7q6Apz.xFe;-]]sAgOFsUSq&zo`'v.a/oOXNC?_KD[dBpGxq9AJz`$==OWqHZ'lB^TU*MKshSaAk30,4M@OCf$xD5Gf.t]LV;_0S]0AD\\'5u,ZE5T[NH'4S9#)mAj(r",
 "Pp*0H=M?lVuqJ5[N@coh)VnlU1i-r0Sqv+R?8pM9.v+dY^bgfJr_BoN0H(W.]svaa_-J(=<;ls0P<5?eCn'q7\\dAqv_MPj?y>,.nZp9M1[yC2,7,h]>+Q0a]hivW-w)5q.D4.,.(<wY,dVH<Iqkprk",
 "fNNl+wocWU$iHwPl,4:$PiU`PHhcSq`9(^CLaP*gJSoe?EIqoHvGKEYJ)[-8y#BYgW&/G[V*g?IUN>I''O't&m&nRKQXgwFV<Q2.XvEp@M\"yvQ!#jN67)f.+4;;VnDjMK]u\"a@Y4?KtaiSvzb4#>%7",
 "nh168nvASh130S2LXJSLN,fqfNb54Oe/^F/YE[7N1HF)fFF>Utqfeo0q^[&\\yMs9KaB<nvxufXe=v>vBVZG?l#:(@jUu.NeFC78_]Z>w']mqlRhOC[r\"h@Q$?[>JD(CTCO\"<XOj,!uvDt5A4#d9g\"f",
 "E'Dk.J]rj4`<xMYOeL7a/=`s!A/DXfF19@l+T@kuO2,DT!m$GMA.)$F=3omTa[fS3?hWZU'b]SUWin$&1Sx_[P>evyJ.?4/TubhMUS$.&--6r_0[mq)vu&nv;FNwlH*WeJR%\\^[%W,H!MCkGV2rj`H",
 "IF-JGDM=;OWjhAp(FWYQFv-5u_!)I&OjI879.iw_qs^A8!Jxoms1gP>J4)^MbGt'bBTzu`%p,1Ucy5$;7m!A#Fr?ist/9ma4FeItiCSa/dtdot]tBOAkJ+,WCU<:V+.+y89+^gM'AW(j39Zi<M<8yc",
 ".eYx^#>iJ7,o]W7)QztIoKw0pY%u0LTT%I6RJ?B!Fm%bu95]vD>LZrN6#y6StX!`7Sqt/kNh@@T\"(gE,C!?^an5ec'hCu5%^bn9o(J,F9HXs9o&2m3qwlG'h@uhmglhwc*]KQsOz-2M?77DhOA\"-HV",
 "]`&1GsW#'hYYf-1IPV/^+_:+&G!ms$`GFydfL4KjHIFe9ZQh^-#quG9^`BlnI-UKSyEHc@Ex\"x&[C)O4c?fU+CD@]=,Q0nJdf<N\\Si>]-)8StBGhyvv-vC'E!`gJSfs*+F<\\]I9*7gpP!(M4&1`5^;",
 "wTD2Iqk3J>V\\NPU6&.)T=EJGdN16!aH.!Ip[Qjtu<D98u2UmHdAOx.w,W?IgcyK(^R-\"z!!4>acbeuCH`Wnoi\"Pgb[RAt\"v=9,&4t9SvP2h<3dN(lS%E@[g2qjQCgG)@e7?Uj&Kqy8mOF?5Wr';B(m",
 "oC3hI9]7yXlt68zuJ'dPi3s,[\\RY&\"I,CC18/I3@O9:/H3Qk9=t#QNmcj*Bs%?fc!?XzD6Xd3O3JJWC1=yd=kmw9%fU)Ln>sge=DB42z4m-0I9zWV5MN/))ZoD=\"FsvHX8LkUUw.C#v-J]DOww#e/5",
 ")_tb8h%o&U9vJifK,`G5\"$`9BhR_=;q-I+)`Rw;wAaz<(\\8CBS$%tCk\"X+uex!zv\"UGia6u!z(Sq,Vc$)ywRsN>5`u9MY_tT('CFHqAQg/IN2;VN(g+MFqg`uSgt%;ZgQ\")sC;xQT#`rGd7L<eldtZ",
 ",,'=qsZ\\u)ef!zzJp4PvRi;TmIA64]o#3RmamVHp>(YuN*T!8WazmPB'+r@tb_@m&]hS^'MJTH7^;;krw7_^j&<+]vI!;O60_iz'\"T3m:viNX&nT\"Gk'Jn.:<YN2R\"Yr:^U\"Cg=[E%+DabJdl*d[m&",
 "g!)7z+>?$G+`Su6J(&?@(.t5((lX=/LN6ea\\));zD<dW)CL9]@b'sS3gXEQwN3QDDpCznM*Vsica++_@O1%NLS)gAu8&1&?L,&C-eP@Db=ynSbPqV:+Y(3&ov?qV!Weg\"xc5v/ooPm4)p(YcOrNLWs",
 "aY$#Oqxpy\"e[d_MM%t-u'+?vWvIN$=rdLF&%E-&kj$0CmbUURuCwKoXo7;]m$OfU0e1#>*nGjNwf!$u(\"ySfK\"bG8Dmp]e%\"K$U/^)sPKcI4Eg?,w)z,z#8R_1='IX<UwPD=DnJo>T$%s2DCo8XD&!",
 "CV5po![>%#^d)Zf]?_Ag)L1e8B)\"^w>?sM;TvM%\"5YGUdktZI;NmO'`&w31,FxoWH=[@ST)OK5rlV7)mX%rW\"T$ZNh_g^Yzeak#)HT]gEN;lc^FsIR)w2SkcBSgOYx/&Y>(u.NyOz$hf\\A7dJvSV'e",
 "P(%_.F:M%gO7;&('`d6%8+@Ub6KEv52`AhI\\;CSODkeOU;!s4D_yrnCB8*=7\"pFNjiO7QM>@'tlu(;5RYz/OPeR.HHcmQr@,UX[zFpbG(wmK\"%o&Q.G1Ej$12y5W:?xyGWYUc`XsA%<r4zJ5J4NxO)",
 "N.h\\YT=fX8ZpcK_q=+Hmpr]+#Jk7r]kKLEpbK?H+R,fbr>FY4A'l7)V49A/3hc%F\"c>\\0Hxg<P\\-D\\P;`1hV7_#+X6(\"8/qCW%ant)qvk#Fx;Cl\\GxE(L@TY?X,0Gj8;+jiB+bk#H\\!vsLT#[OU.4E",
 "RHb&8r/9TeuUo1SC/]rsBntjDktuav@5e*]nm)^9J'2?ODhg)-8nWOzhm'23Xdev%HEYu\\.od<$QP'/7WvE;5Wb_8U?o9oUGd2n6h8h-2=eI:O5ssX*Q>-en-bgHd0cHZN4]Y?9hN\"ntdu_Ok>4qGL",
 "JaKS84HNk3&-?EK2O'nO7Gv5^x75;j_zw8Z(]ay]LszPy3>)+jwwqz&I#b#,vt(1>%YE+?@bAII4n8Si&@]e4$U:-p?MVSI]:c].+WI?iOzWd.DT.iGKKf;e0DKcyhg:AS/)\\tHrT^]R57(&0#JS!G",
 "&E2=znbnzSD\\S_eKx\"8B)_*wpA%hxNI]b+\\C[`&#]f\\<G]?qafKd0KW*`vrja%;m/L%n(K45X:ZXh$ek9EMomPh(%WdB0_@y:o4/'AQs!cO:]F-o\"]_#/f$Hv^Ixv;`+cxCCIXFwd&+WwA>Ye8t!)[",
 "jfRFN=i[BbzZQ1T*\"Ev_Mu!t4l:uLq=oRZ0b)A*Bm(H,gM&KP9,MGdXST)m:uo^Xa4^OZ9Gav(C8kIe#Bb\\w:5i<0U\"@cM8awEd#env-V4W(=RZ$C5lQ8<O5A>j2[0;?ZOtYSo,p!y^]wkz\\]=6Mk>",
 "7)lj3+/^Bs-&=TPhjAX4Bn(Yy3S!d!?YQ]yNNllAw$L8]m9s>'`8h)RF[a\\;<huV`73y/raW,o3:N\"q#;,P)8tMT.F1w4ds0\\d5TKeiFOt`E^.eU3>)sf$pUvG<@;Ye1t)MQ6%!FI'nkx's113>q\\T",
 "chY8h'z3/mWVxPI+KpaS,YF\\KWGw](X1,,2#ME=q%n!qPUa`ag5[YECT%?-;mlrMp++H,f:eojypYaegqC$ZXh5dAlko!(u1R]0]%o0a4':.i`f#-qz,Ron@3$ZFHPN!e6JM(_1Y\"PBV9g&H/q+nQm",
 "B7TMYP]G#BS>nhlfB87=hC4KP-:+QD`]m7?AvJJ0,ImwG>Ht0<,(l20%FKD&OjDRoW0%42pgP6*GLJ``txYoq#:jLb`TafDFH[;ux@Ep\\#O8LAnM2BwR,Zz@XXS$:Qle7w\\<ZD&YabF=w5AgM\"%\"kE",
 "CtvDyIJ`%:?eo8%z#*.*:fY4,(ed+%o$5V7eLl9q;Sp]r*EV\\&[L?sQy8(JU13xqfR^L[@$rx]w,xAi\"2f:Q48QyjKNQ]/`'rVD*D8<`.N@uH]lXxU:h&>1xm/eEYJYth>J.UVS4YFX[j`n<Q13nnp",
 "<P]TfRSfBETc_0$uYhD\\o7z)cNN*v$no`k9q:Y(hFglT'Jw1sx[xV$H#YE2S8EWbMeP97>9)ZiS#d_Au;Vf@xmj0\"/l(AX'nKCL*l:#i@q&L_bq$9GK.RXr+=]7ty=-m31p-92a%2]\\/_Nk\"^g7YU;",
 ";\"S+XvuQ*o,DivT@_y/\\=HhR;=#+E>GTI_k>Xu4v`t[Z,IDX%F/0!`f>()@T%Ok&4D@KY>//*Y]*Z,1cXVvBCR4/>'_V_l-/>a]Io&'_0qd+ABEE$(F8Dk/*JRdAv-Dmr2[J:lDCA=rL\"sMih'G6\"Z",
 "L&$1>`:fb7Z`Z6N?w:aR[t%mCTT)X],t9hg.csLqC\"A0K3R'^jL.e<Clh]0iL4$!`f7n2klN+3yn24PcnFS%^7o!clrI'90Z.Uq#J<vzi9s3dcc^W$B0lGmk7?YRVP[K>>N7Hrx<?H'b;3QoI-[/zT",
 "ecvnNe7&wy.ea3-y9Lt7z_;J4g(A64*IhccKJ[mQP8FF(.RT)87bX2GE(_V.4UhKi&uSK3@5qzqUKYQ,W>c5=h%i0-FgBx8PZh=GA7H]knr>B(pjV@->)6*,6[=K;OxM?VD`f!MR)\"p^UG\"]4vRhc>",
 "As8Vc'!%<nijgk4\"lZp3PFzm8nPfX?3NLwe]^B&#MZQ=e>tD5px\"0TMy+'5]#^+7Mr#0P9estmQX5.:!D*5=IG0JZw\"H`c%j;6[,BE5DuBz.c?QgCCkss#aK5/S^va9d6Ft_S@.&-.3-+;Nv9BK%Pa",
 "T!qw5VMChn4b5ql)uLGE0h4kJ^\\iX&)ZAEEa&]p%G.[)Y(83#8xy<yVjo=_#EHssqxAIp3&U:cVF,\\8<@u-[sitzUW99GRlI<ZVHzWaF^UC!z?`q,&<t.p]&kP:fCxij8d]\\wGr/i0I<rDw^D6sy+Z",
 "^0M@9hvw6q5:EJDmX`?E,*:WcC>p/qrOkP(f*u@C-,8rB=69pi&KO?ZH22)Kpz4XoLf0RuLt?NYM'e-$nq*4WASie-4>CIz7R;00NiIY_-]<X4fi,Xv$637Ds``#kZeN[GLic(`-U/>>PS!#^rj^e%",
 "'>#/t(+8vO?+C51$t,pWchoJ=kN\\ThazK_8Y[rDW\"V&x9iB!!N5ZP/6u)K>'Vt;F_k&EBrb4J#hb<d-Rnp-Qg`d\"m9#U,z^WcygVwEXwe0Vv!]o\\#aIe?[bW`WUToJkUnzaCDfc.0^a4aW0[+6jZ>\"",
 "L+h5DD3$`@9pv2I#V/ff9v/2(,g3cb?@zBZiOa8MM0/DV]q#,`hr'\\`^G^]osJj4b)a,S4<[lNQb2B^86d(,a7gR#@b)(u+8YIs[pv_0.Eb))IyapUk*Ms<=]T1*=v?X@n#8v;c0\\c(#$/&nx+-?=u",
 "yj%@aE'_XQ\"$XJ;xK@/zax[M/Ql\"2nHwi``#5h`F9w9%:`AkiR.<uj>s];>).fv*l/<&Pe7+dj@<aaqT]h#*7RucU\"d/=B8OW_I)H)q&>p4uQ7P\\T23:JrjaAlxY*FR?Zm&lWsdxuo@#69\\`M))khE",
 "m6x#iI(YiIZWTrvCUR&9jnw6`3Bdp6r@7,Z,/RKst?jVvf>d!jG.RM1flwI*Hl3_f6vO@LWyXc?3L<CdWCuR<`]g;S^;`d&xTz,$]cV#1ZZh_)Y]\"k[,`GHAy[P=N@,74u%%*v`sQh#j#fGEx(-jw)",
 "EUhex%W2)\"je;$74\"/ZGPO7%us=*pI'3AY>x/4rzG,?y'MILa[aB(T/.&5,nQr\"B-*pGoselyku84o'8)7ykvwTCk4?g,saII4K^YO)Ahs;Ms$gd%@S1jOZFwB7,mffoL9^bu/&AH@?7EO5AtqEmD]",
 "@ufF+`i1Y(WP.:BK62wd@Z^PHpMR</]4$FBjF.8C*fd(I0R\"z\"w22G4Ah_s,I\"klSnW1yV^P%$fHcE\"oJ-w8+0x+n`01\"\\jn@NX=S5>Qg#Pi@G8J4r'c9Mgh!sxyTbV7X9C(ox$\"=W*'ZD'<E(&i9z",
 "eVq';eU$0UG2\"SN!lb\\FzR]EKfgf:-/kwxXBWtZWiT^jR&5i[\\E*'O2w8y^9lL;QcgdqR_/vd<N<dz&/f#@U/t!EDIp:>Iw,?J8r#-\\IX-uqR/%X@ryxB:8n7EL6Pix?iz0bw&GG8Q6Hk@Y-0US_1U",
 "#6P(zrb$Z+5$$jX7P2$,;o^oQf&ilYc5d=N^U8evTx5;iqtp1].(@amwO<Rj(um34lG_P_NrQc#:.L@/H1xXi_eCcS1JlS*&f,R99ZrNR+YNH81#)xF-pBiih8N=C8QB'ni_>E>g&MahpFWlMf:Fjy",
 "AtqYif7X4\\#B_f>/!c0APA\\#/OnVhq3q[bS[X)bY!vQ;WLI9<r`k511qC8`]y/PU]@;4Caj7vo*!5p#8n-m'WX=@Zo<d?e](RX_!hrlUm`D2MXssL(^sCuWV<C`p[^<k.a9t](F<T_!5)G[0?`eWJ#",
 "L-g/*_j\"9Ip;'adOkCi+=?l.o/a'!;vN^<U/!n!'Xa$>M4Rv2ETd%>!W&?QeDjmK+zL,q\"*@kRsX5x#ZPohz@Tdb*i&NfTeC4<k#T'w8m45iPT+Ho',b(&\",U>Kt0U:S.Y(*u4NLv`h^G5CoKw*AQB",
 "Gi'A08BQNvF'Iu(RmB]#&a\"^v#=rgunIi)panua6/=9XkEk$a;m4`O)A#v3\\N=<gJk]$`K!Srg\\[R,?dE*inps!=odqA>=JBTkI!8rGvc&?L)AgOf4AkwODs+i(XLp<wXpg:O)gMIinHqCdnVw79BC",
 "]i5\\a7!%:F]<rSuCJA[W4LNOZ)iJi+nNf!KR8iN:dM:2GbigoL\"1:^(Dbe&'#To-MKhQ']CuSZXP'>7Ai?!2tNU22yM*CQ=p7KArk8ZW+E<bLe-<nF\\0Gj!#^]yt7/uqqS:/fG1Gui8eAbZBFF!J\\U",
 "ODR3hH,y7wjp;I7-!)GwCHS/I7%#Mp/$'nj^H!5Ff>fxa(LY&iK^a6FHkg-h5fF?oxw2,zzyYz!DZ/,A.vC*Ltxv/$BrnC'1X[G+a2a`#`6tj^z3M`k\"zJ\"V10TAr]xoB)jBlN3\"6b>).N`%Xgp5cq",
 ")xo[!^N8^=1;U0:$:,i^=PRyZ=d(tr/DeJ)A`9!(aEtnGn8m$qnr*pnX7qK<?^crnXjJ#1]7PM]f8[dUi0b%T;3mxKk-SHTALJP!gIb:Q;JaG_mAP?Aq`GmGc&v.Vn2wD0b6UV4mZrB(IQ`aT=A*>@",
 "m9?]xE\\oNej9hkjGKE%1(CMt,V)'/g@Ol[(5n:Uc5hcMlv`0hjIY[jT&,rq>TpdxY[AF*%\\;C/cqEua(Tkjc`3?:87_iJAz+u!jxqx`rm[QZ1jP2J*;rquuMg<@I\">DABqPAdA/Qu8%oea:q>\"5*Cj",
 "v&'DoY\"-ne#cn,3<4.a;PcydDWJ]a\\$5.w0qtG6gf7G/2-XF_ZauUki[_?yOG]6`nLle\"^\"'4ske^X41!tfQk!g<>9N^d\\A<&uMb^.+2'T(gjXR.;O9OQ'jx_gwA%pd]iqcV>)Rza8Ns\\Z'6_a1D.v",
 "Dy`/Z9p(@fG\"50bp*fGl8%Hn#gl'xR!]q+ZbrjD_?om?+X(6sywMPxK5w`RwQR^ypHjh;tw![OrwE^Vo1$GG+M/G7P^U[-^M\\[y+dM[`_Yjv<JiN,Sh4YM5m+CoG&\"vCYQ>!>7m+_NU9#zD$Iws]DB",
 "BoJ,mp46Q>F7crs0/H%uochAxQ8eMY85&Cx`a9oDx@6UfVvgsV-&IOh*nLCtp+?,(;c&:)l*M`l8wJIsb&<O_&CwZ'$Y9+s\\O$?;5JS)q-=E>(IB[e?9E<W9I,kcN=&N[HP/0iIcgB9ULOEaQJE\"i-",
 "O[wU[gL9r4>=hhQjN:WbIBWCjGpnlCGXC&4x\\p.t4Gr-GXJ&XS=DlMf-Qd1)MZqyR_ATAw?$&ejKK/jukED'@m7s_z3o#Om#ko38,Jm'BNE)Ba0cEhYAxig7SW,ct9N.i<$\"@Ej/zsh_,jF01tA)G1",
 "8?w,n&XkUno+qCdU+Qm=L.G^:ICug&dJek6=C.j+^HE_;F/@1^huN<s/^Y+\"6VVpXy(_iV)p\"3eH*XMPcx-@cpga]*])<lv'TqYJ+kw$toHJ,pi/8j0\"1M0eu^Esy0UEoW=Nmw3Z(7(-ZPI1_y\\S+=",
 "moq9]&kG=:RLT,jaYzu[$%uo\"26,r#NF7y(jA`+scO>'f[!=o=%STU-L?pTdg/-.\\;8et>?&)2#rP;\"4b>2dEua-IV4H>f]XtbxygY0Cjp@[5f4bzGx^5llmtL@SxaZ4Uc/j[[3YI^?N$sSTwLB(D_",
 "\\H6!-69SOtu*E\\TQ1f=geQJl\".\"Tnc0eC)P\"Zg&,a2lg&_R()`?U/q]PLU=pwC3eJV@]Az%6%\"x-M3V>v4em;o#XDq=l$)c]HJ71N+1^TInnBWl.n[S6o0-+-Wqgq@AOv\"Z;irB@\"KAf(m\\]1-xDmM",
 "Y,j9r<)+KNT+@=_2+M(tC'5TZdxKt7x25hrJD/Y)9e$Pu]A\"PqzUn1Lp*bm^sF0/ZDg7,+72hpj3)>.LE`NZLRQ9J-YyJ.yn<=Fy:K/0dS1Y$!pzFVz#LoB36]FLQ8.PSCio[2;](v+W2;m#l/AG19",
 "zP%PW[+Dh/tMA5\"w`\\$I!=GbpkHW/,9vCRq<75\"QC6>an1i9aG8g^;:Yc*osZZy$gR)'#tQZe?@.Nibq(Q]^C',\"@@F,uRg:GMXO5PULJ\\#(:rlvy2kQ8O(JA0Au4w:<&'*g?<tD0zOkeDD!kT[88j",
 "b%,lL4y,\\'/LzIi+0KyQaWYP:%z;5<FeDi$EnzaWEd_\"ZPOab_[J$@ofMk](lmQf4yDF8$Xr&]Z\",&xJ;/rc^a;+?09h5xsh[\\cMf6Ae(#6&yS8C<0D^?IUl(9:UiAZUuD'/_bLZttrm[D^niBywWm",
 "vGKDEskG48p*HN+MZG\\<sN5^3CHmST4dn<?mI%yuWaGqx5^x-'V;7;*)X$@1C0Ktc%\\eObbA4r/E2j?#pn=N^Vz72[&arX^CGT1l:I9H9UK+s#h2wgvdNBAt7c?sv4q;[HObL_UleP\\6KN\\G*T`zXE",
 "Q0sv(n<xHT9hE_^FP2mY,ZpH\\7v8N-I%2ru5X9Ws[4II>zaU#((pz-O-XU.?X8/HI%f9d-_+%YHk.4@ORd*y+EYVOy;ELc'-f(yk,_kfcpx$1'J<652%?Imp$#T6w@Bn<+>SB*Ptf\\VGtr7`O#e_]9",
 "N.;x0ZNqM?/GQ)K<vBJ;=v:?/vP9K#j/u5kHBCpF3Uj/Gu64#Hr/7ZS+QW3EE/sN)lsyQZZqYak1aSv4z9IN)]9UKu3]AAl\\;K)Yl6S>dAuIU$]TRfnLRk6mRIH#pUP$,ByByQ7jFqIR7Ie+1R'fl/",
 "-t6_^0F@I6mOmvFS`Zh5Qt%3G3t6KiVqQX#7K01+cdxBU+_&e.wQf<U1l;-8TNn.**eBpyxEo$rM>-x@\"a8y61LGP[_0H)Ep`@68N]y*_?w7;$k\"PZ#jY3vJs3O//W&fA!SIZ<wYH4m,w*mz!^mOl9",
 "8pO;<mT\\tg=z9>w7%%VhVB[RQPa!y^YPGK7%rX0W+Fq@+jAj5(U(b7R7'eU1l(,Y?EZ1-v4xU2EYMqqc7yTmVsBI)QZd4@V8/3p[c;qVPRfw!A\"XmDvu$Iv5%1h^EBp7SON.Jc3I\\l%$>cL1R4;V,5",
 "N^oVTWZL7;ZKrN(qJ4^hB7KL76=1z+>x:tB#P:`f^cpVI3+P71N!]Eg$/0sqom\\eE:9c*Q'wr7.F\"_o]A5@)6yw0D6&)Y<EBoGAzU5-hTtjQ$N:N&c!Pd^r!=95Hlg;F?uc=&5z>z/y$EcY63HKsk_",
 "BPfy(bc9K8\\,n=9ri`'^NnN;B@H\"'G.3`R,Thc]_f%5D2rZvx!UwTSK7)HnKkO+^kCbFNFl;m7^Tvzh?lv5>eszlRYuoilI`9_&kxR]cz&_irx0Qg[Z:\"FCKSY@R;oHKUY6jx@(7K$vN+&#,hyG\\\"7",
 "Hn1)WBff3>*mLLa!JD\\OTO3%yDFg4g&[]=d\"Q?p=KV>!_IiPZrdga\",\\+n5U3?h$!/7*=VzSgX\"E=>x'D)wL;SfBfF%Lx=w'7;+BVL\"f3n,y>@M(W@z\"z.x#fxNPi5WcGMxZL4*%v#>+<@b1a<(2\\*",
 "r$nLqyAyW-(kk\"W#zP!`Gc!o6a6@_QJntyg`eV@*.)8.TK$9^p<,=J;@f2phyK>#XY4DD3UF.;bk480N#$Vy2q/&IoCBIe/-]6tCtRnRjw#e;3Ro)q@,;>=#nWe-\":o.BFERXoZCYA>]cPJBI\\\\^t&",
 "G+*o`ka\"A\"pJKq`TI7oB&UXxCbY]'k\"O@-+k3ne4IoCV+1qr[jJe0159lB0`c7fK*mFWBuUj-GGf<W\"MEJ73;_HvFER-7_^W0uhxQsCUFYA@9+@R1u<O;@^w.-&i!>B>F9FNG&Iuji+3@`);AK\"J;O",
 "ox/NEy2m.qp!)7BKR.wQ;']>0B&md[wC4NOA-,&\\TotN:bxj-;mTnA5]%/Soj2.3i)E;tH]nSY.v`q)+$UN4+'?fZ=u1z(\\M[p;s;,\"c>7D/nGYEoV<QQWCrwf=:w(u'B*H)an\\Q)Ye!aM\\U]i-<=7",
 "H^MNQ:fPj0IP'e9=O'FHVlz2e,^m?2lFHv^8'EKfq3,MAFk>ROLPy8U]dAFD:Zt`@uIO?$\\+QInwOx%']2?MEN5rdT:n^,<y#OPC1\"pjlt4i`R9^!ofzS3?#!0;E?NzB@($b$nETTDA\"DfE+#kjC\"U",
 "R^`urvGZI_s0;/U\"_q%Pfe!Iidz>bcRzg`oz4mn8YtCV(qHn9:C</.)GzY66zZ^'@Jkm!(B5%W!t+qbzJp3p!?\",jG8PR4tv_(7y%/soekq2WRQd*!LL-'SFfLGvJN/:^0E\\+J,Q$Te5uPNGIWsLAh",
 "$z^xyFyK9kEaV]N(C?qG/*]b`-pEKvmteTLFX.EB_2V4i*f;#fCpn9tmlbGkz,z0&3+id7z9CHk@_(q6N(S.SIO^]+.v1K:D%\\&:)q('OQp`NTT<%f=IId*LfhjfL$j?*L906fNmT%xSsHf@R/IRG?",
 "QlMzxgl/WURZI]A5V@?/@MbDpdb+MQT4b^c!I8oB(siy8rZaT3B(,f\"\\E(G@eS:dUqN;.8W>-7eMcl6Wi@T)O@,)DIoSR,Y'wombI'!1f9\\p2l-oYwPstkt\"R.N(c\"lqKIVTVzf]Xr?13\"T]L&*?8`",
 "FU*d_?AfKC\\h@?$+d'wS+*(X=@Q\\=l5*dRMl&DF1!zg8$J47/zl.&-C[#U4`6kISA;U_/%</6\\f:[s2=i`b7$TRvuK*wZXK$xVx(ryfY3QSG\"EQ:cfgT1aarF*foUQF<d\\W:ljBj!^f1%6NZ\\6S>[:",
 "e/kG:/Bo7,s2OExEE%SEngAbOWsgwQzZk(Tl7g^)],]m4[,[p.XZ<!SS:9=Zm.G`P`DrJAJlg6!`T!W\\S*\"a*O*/6&-y?%)%D%X)6,9s05;f`8P8F6Z:oW6J]z8/MJG_3^#0vun&a.14bC*r/MO?=n",
 "<iG0$sJNM$n:zjGZA_e^Y*BmE8hEE(y<-CyD>$W19G(hJyd&+dPO+u7ZnQYV?Oj8y(Z9RB0+UapUgf`iU^>d0*%7sV'Ka+_ad+dI4NS4p@(92!E3%f:<OCwuwI#Si:wL&7sC;+JfXji)>fNs\"Q9CFm",
 "E'lscxB@S3P\"2Ga\"Na8H\\/LgnV-[EQ\"*>7SH%(Ctew\"oPLk<xMik3eLKo%Z8h;_9!D3DBZC$S![[qp6UOYuNs!fn>J)*?iUs9Y(16v7k(`=V2\\z`(Oa1'eOGD4B1x<tN<q7\"mMxCs46?=F/cT(]<><",
 "a\";;v[tH\\SxvkY2WV@\"S)0yiSw350TKq&m8v![UC>\"3Fix92@UlN\"EQBzO8UU3RE92\"s6[>;D\\oA)Daq*D8[;4v);<W@[3ylhrkPq7'[Bzo!_y+;3?jHRfYVcTI*%)5Lat(=l!nxO!qJLBZ-fj'6/x",
 "oWk/iXymfsJBuvS:wb,swCGi':&?%H;-<vv9']NR>i(C*Ye&^:*3M;ri\"O*hPZE+n8F$'v3\\Tr#dfAmT'e!yI:z@kNM:rL(+Y*'/Ah)W2P2*k\\e8\\q<xQqtFAp*rHJSgcZiS&rrp)_J$Di'u'Jx%D\\",
 "ZR_@Bn\\E4sVDE<tFoDlo^GG;%mzX=$yAT>%SUHX,+J4QU+TeYv(!lv3;4bMx[.pFYI.%*R#F8/(3=cvOan51U@ycoVm50>5zh\"[9e&3O#7f)xq'iccR]s)OdNwU\"J\"oX+(DmC<O$YaM6#D)Aoq):#5"
})

local FL=#FIELD
local function E(i)return (string.byte(FIELD,1+(i%FL))-33)/89 end
local HY,WY=43,44
local SKY={{54,54,142},{65,60,161},{82,64,177},{108,67,184},{139,70,181},{170,73,169},{201,78,149},{226,88,126},{244,105,108},{255,126,91},{255,151,83},{255,177,87},{255,199,101},{255,218,128},{255,235,163}}
local POS={0,.07,.14,.22,.3,.38,.46,.54,.62,.7,.77,.84,.9,.95,1}
local D={0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5}
local function sky(y,x)
 local q=clamp(y/HY,0,1);local i=1
 while i<14 and q>POS[i+1] do i=i+1 end
 local f=(q-POS[i])/(POS[i+1]-POS[i]);local a,b=SKY[i],SKY[i+1]
  local d=(D[1+(x%4)+(y%4)*4]-7.5)/11
 return clamp(mix(a[1],b[1],f)+d,0,255),clamp(mix(a[2],b[2],f)+d,0,255),clamp(mix(a[3],b[3],f)+d,0,255)
end
local function skyRows(y1,y2)
 for y=max(0,y1),min(HY,y2)do
  for x=0,W-1 do local r,g,b=sky(y,x);Pixel(x,y,r,g,b)end
 end
end
local BUILD={}
local function makeBuildings(n,layer,base,step)
 local x=-(layer-1)*3
 for i=1,n do
  local k=base+i*17
  local w=4+floor(E(k)*9)+(layer==3 and 2 or 0)
  local top=(layer==1 and 28 or layer==2 and 25 or 22)+floor(E(k+1)*12)
  local col=layer==1 and {78+floor(E(k+2)*24),58,104} or layer==2 and {58+floor(E(k+2)*20),47,83} or {39+floor(E(k+2)*16),39,67}
  BUILD[#BUILD+1]={x=x,y=top,w=w,layer=layer,c=col,shape=1+floor(E(k+3)*6),sx=layer==1 and 3 or (layer==2 and 4 or 5),sy=layer==1 and 4 or 5}
  x=x+w-1-floor(E(k+4)*2)
 end
end
makeBuildings(22,1,100,1);makeBuildings(17,2,800,1);makeBuildings(13,3,1500,1)
local function roof(b)
 local x,y,w,c,s=b.x,b.y,b.w,b.c,b.shape
 if s==2 then Rect(x+2,y-2,max(1,w-4),2,c[1],c[2],c[3],true)
 elseif s==3 then Line(x,y,x+floor(w/2),y-4,c[1],c[2],c[3]);Line(x+floor(w/2),y-4,x+w,y,c[1],c[2],c[3])
 elseif s==4 then Rect(x+2,y-3,max(1,w-4),3,c[1],c[2],c[3],true);Rect(x+4,y-5,max(1,w-8),2,c[1],c[2],c[3],true)
 elseif s==5 then Line(x+1,y,x+floor(w/2),y-5,c[1],c[2],c[3]);Line(x+floor(w/2),y-5,x+w-2,y,c[1],c[2],c[3]);Line(x+floor(w/2),y-5,x+floor(w/2),y-10,c[1]+9,c[2]+7,c[3]+10)
 elseif s==6 then Circle(x+w*.62,y-3,2,c[1]+3,c[2]+3,c[3]+4,true);Line(x+w*.62,y-7,x+w*.62,y-5,c[1]+9,c[2]+7,c[3]+10)end
end
local function building(b)
 local c=b.c;Rect(b.x,b.y,b.w,WY-b.y,c[1],c[2],c[3],true);roof(b)
 if b.w>7 then Line(b.x,b.y+3,b.x+b.w-1,b.y+3,c[1]+4,c[2]+4,c[3]+5)end
 Line(b.x+b.w-1,b.y,b.x+b.w-1,WY-1,max(2,c[1]-4),max(3,c[2]-3),max(8,c[3]-2))
end
local WINDOWS={}
local function city()
 for layer=1,3 do
  for i=1,#BUILD do local b=BUILD[i]
   if b.layer==layer then
    building(b)
    for x=b.x+2,b.x+b.w-2,b.sx do for y=b.y+5,WY-3,b.sy do
     local k=2300+#WINDOWS*19
     if E(k)>.04 then WINDOWS[#WINDOWS+1]={x=x,y=y,wide=b.layer==3 and E(k+1)>.62,bg=b.layer==1 and {52,45,72}or(b.layer==2 and {39,38,60}or{29,32,52}),wake=1+E(k+2)*24,ph=E(k+3)*tau,per=22+E(k+4)*58,duty=.72+E(k+5)*.25,cool=E(k+6)>.82,soft=E(k+7)>.8,layer=layer}end
    end end
   end
  end
 end
 for i=1,#WINDOWS do local w=WINDOWS[i];Pixel(w.x,w.y,w.bg[1],w.bg[2],w.bg[3]);if w.wide then Pixel(w.x+1,w.y,w.bg[1],w.bg[2],w.bg[3])end end
 -- water tanks, dishes, aerial lattice and two restrained neon accents
 Rect(39,24,7,3,48,43,73,true);Line(40,24,41,22,72,62,94);Line(45,24,44,22,72,62,94);Line(43,22,43,18,92,75,111)
 Line(75,26,75,18,70,62,91);Line(72,21,78,21,63,58,87);Line(73,23,77,19,63,58,87)
 Line(101,24,101,16,66,59,88);Line(98,20,104,20,66,59,88);Pixel(101,15,255,104,155)
 Text(57,31,"LUX",255,54,186);Pixel(58,32,255,163,227);Text(111,32,"BAR",75,222,255)
end
local function waterColor(y)
 local q=(y-WY)/(H-WY-1);return mix(220,74,q),mix(116,65,q),mix(151,142,q)
end
local REFL={{7,255,184,85},{20,255,120,112},{34,255,221,132},{48,255,165,194},{62,255,101,175},{76,255,230,151},{89,250,128,214},{102,255,192,111},{117,119,226,255}}
local function waterRow(y)
 local r,g,b=waterColor(y);Line(0,y,W-1,y,r,g,b);local d=(y-WY)/(H-WY)
 for i=1,#REFL do local a=REFL[i];local k=y*61+i*97;local w=1+floor((2+d*8)*E(k));local dx=floor(sin(y*.83+i)*d*4);local f=(1-d)*(.25+.27*E(k+1));Line(a[1]-w+dx,y,a[1]+w+dx,y,mix(r,a[2],f),mix(g,a[3],f),mix(b,a[4],f))end
  for j=1,4 do local k=5300+y*13+j*31;local x=floor(E(k)*134)-3;local n=1+floor(E(k+1)*6*d);local v=18+floor(E(k+2)*35);Line(x,y,x+n,y,min(255,r+v),min(255,g+v),min(255,b+v+5))end
end
local function water()for y=WY,H-1 do waterRow(y)end end
local STARS={}
for i=1,82 do local k=6200+i*11;local x=2+floor(E(k)*124);local y=2+floor(E(k+1)*25);if not(x>88 and x<111 and y<19)then STARS[#STARS+1]={x=x,y=y,ph=E(k+2)*tau,sp=.23+E(k+3)*.82,v=176+floor(E(k+4)*79),cross=E(k+5)>.82,warm=E(k+6)>.72,wake=E(k+7)*12}end end
local function stars(t)
 for i=1,#STARS do local s=STARS[i];local r,g,b=sky(s.y,s.x);Pixel(s.x,s.y,r,g,b);if s.cross then Pixel(s.x-1,s.y,r,g,b);Pixel(s.x+1,s.y,r,g,b);Pixel(s.x,s.y-1,r,g,b);Pixel(s.x,s.y+1,r,g,b)end
  local a=smooth(s.wake,s.wake+4,t)*(.82+.18*sin(t*s.sp+s.ph));local v=floor(s.v*a)
  if v>30 then Pixel(s.x,s.y,s.warm and v or v*.78,s.warm and v*.84 or v*.88,v);if s.cross and a>.86 then local q=v*.32;Pixel(s.x-1,s.y,q,q,q);Pixel(s.x+1,s.y,q,q,q);Pixel(s.x,s.y-1,q,q,q);Pixel(s.x,s.y+1,q,q,q)end end
 end
end
local function cloud(x,y,w,d,r,g,b)
 for j=0,d+2 do local inset=abs(j-d*.45)*(2+d);if w>inset*2 then Line(x+inset,y+j-1,x+w-inset*1.2,y+j-1,min(255,r-j*3),min(255,g-j*2),min(255,b-j))end end
 Line(x+w*.2,y-2,x+w*.45,y-2,min(255,r+24),min(255,g+18),min(255,b+20));Line(x+w*.52,y-1,x+w*.78,y-1,min(255,r+18),min(255,g+14),min(255,b+16));Blend(x+w*.31,y-2,255,202,179,.28)
end
local function cloudLayer(t,y,sp,gap,d,ph)
 local off=(t*sp+ph)%gap
 for n=-1,2 do local x=n*gap+off-gap;local z=sin(t*.08+ph+n*1.9);local q=(y-8)/23;cloud(x,y+z,31+d*8+z*5,d,mix(225,255,q),mix(112,174,q),mix(184,151,q))end
end
local function clouds(t)skyRows(14,29);cloudLayer(t,16,1.65,75,1,9);cloudLayer(t,21,-1.03,84,2,31);cloudLayer(t,27,.61,97,3,54)end
local function moon(t)
 for y=3,16 do for x=90,109 do local r,g,b=sky(y,x);Pixel(x,y,r,g,b)end end
 local a=.78+.22*sin(t*.31);Glow(99,9,10,255,199,244,.24+.08*a);Glow(99,9,7,255,231,188,.30);Circle(99,9,5,255,246,199,true);Circle(102,7,4,112,71,174,true);Blend(97,7,255,255,225,.62)
end
local function state(w,t)
 if t<w.wake then return 0 end local q=((t-w.wake)/w.per+w.ph/tau)%1;local e=.035
 if q<e then return q/e elseif q<w.duty-e then return 1 elseif q<w.duty then return(w.duty-q)/e end return 0
end
local function windows(t)
 for i=1,#WINDOWS do local w=WINDOWS[i];Pixel(w.x,w.y,w.bg[1],w.bg[2],w.bg[3]);if w.wide then Pixel(w.x+1,w.y,w.bg[1],w.bg[2],w.bg[3])end;local a=state(w,t)
  if a>.04 then if w.soft then a=a*(.9+.1*sin(t*.51+w.ph))end;local d=w.layer==1 and .76 or(w.layer==2 and .9 or 1);local r,g,b;if w.cool then r,g,b=220,242,255 else r,g,b=255,207,91 end;r,g,b=r*a*d,g*a*d,b*a*d;Pixel(w.x,w.y,r,g,b);if w.wide then Pixel(w.x+1,w.y,r*.9,g*.92,b*.95)end end
 end
end
local RIPS={}
for i=1,92 do local k=8100+i*13;local d=E(k);RIPS[i]={y=WY+2+floor(d*(H-WY-3)),x=E(k+1)*W,len=1+floor(d*8+E(k+2)*4),ph=E(k+3)*tau,sp=.16+E(k+4)*.4,dr=(E(k+5)-.5)*(2+d*6),br=.2+E(k+6)*.6,warm=E(k+7)>.7,d=d}end
local function waterMotion(t)
 for y=WY+2,H-1 do waterRow(y)end
 for i=1,#RIPS do local p=RIPS[i];local pulse=.5+.5*sin(t*p.sp*.74+p.ph*1.37);local x=(p.x+t*p.dr+sin(t*p.sp+p.ph)*(1+p.d*3))%W;local y=p.y+floor(sin(t*.23+p.ph)*p.d);local n=max(1,floor(p.len*(.5+.5*pulse)));local r,g,b=waterColor(y)
  if p.warm then r=mix(r,255,p.br*pulse);g=mix(g,202,p.br*pulse);b=mix(b,104,p.br*pulse)else r=mix(r,177,p.br*pulse);g=mix(g,210,p.br*pulse);b=mix(b,255,p.br*pulse)end;Line(x-n,y,x+n,y,r,g,b)
 end
 for i=1,#WINDOWS,6 do local w=WINDOWS[i];if w.layer==3 then local a=state(w,t);if a>.2 then local d=.18+((i*17)%73)/100;local y=WY+2+floor(d*(H-WY-3));local x=w.x+sin(t*.3+w.ph)*d*4;local n=1+floor(d*4);local p=.6+.4*sin(t*.37+w.ph);Line(x-n,y,x+n,y,255*a*p,153*a*p,66*a*p)end end end
end
local function quay()
 Line(0,WY-1,W-1,WY-1,37,35,59);Line(0,WY,W-1,WY,112,65,82);Line(0,WY+1,W-1,WY+1,174,91,93)
 for x=2,W-1,8 do Pixel(x,WY-1,201,113,65);if x%16==2 then Pixel(x+1,WY-1,246,175,87)end end
 Line(3,WY+3,17,WY+3,7,9,17);Line(6,WY+4,15,WY+4,4,6,13);Line(10,WY+2,10,WY-3,21,21,30);Line(10,WY-3,15,WY+2,49,36,50)
 Line(111,WY+4,125,WY+4,7,9,17);Line(114,WY+5,123,WY+5,4,6,13);Line(118,WY+3,118,WY,20,20,29)
end
local function birds(t)
 for i=1,3 do local x=((t*(3.1-i*.53)+i*41)%(W+18))-9;local bank=sin(t*.87+i*2.1);local y=10+i*4+sin(t*.22+i)*2+abs(bank);local s=3-i*.45;Line(x-s,y+abs(bank),x,y,24,19,35);Line(x,y,x+s,y+abs(bank),24,19,35);Pixel(x,y,14,13,24)end
end
local function plane(t)local x=(t*1.4+17)%(W+24)-12;local y=7+sin(t*.09)*2;local a=.5+.5*sin(t*1.7);if a>.64 then Blend(x,y,239,52,66,.25+a-.64)end;Pixel(x,y,126+100*a,31,48)end
local function clock()local n=px.now();local s=table.concat({n.hour<10 and"0"or"",n.hour,":",n.min<10 and"0"or"",n.min});local w=px.width(s);Rect(W-w-3,H-9,w+3,9,42,34,72,true);Line(W-w-2,H-9,W-2,H-9,255,153,194);Text(W-w-2,H-8,s,255,241,193)end
local painted=false
local function permanent()skyRows(0,HY);city();water();quay()end
function draw()
 local t=px.t();if not painted then permanent();painted=true end
 clouds(t);stars(t);moon(t);birds(t);plane(t);windows(t);waterMotion(t);quay();clock()
end
