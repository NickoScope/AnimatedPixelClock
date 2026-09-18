#pragma once
// The page at /fx3d: the owner's remote for looking at the panel with the
// glasses on. It only calls /api/fx3d, and one request at a time, because
// overlapping requests are what drained the internal heap on the panel (the
// integration session's measurement, docs/drafts/27-heap-block-experiment-
// 2026-09-18.md in the knowledge base):
// - a control's request waits under the control's name, so touching it again
//   replaces the one still waiting, and a toggle's query is built when it
//   goes, from what the panel last answered (two quick clicks: on, then off);
//   a slider's value is taken when it is let go, since an answer in between
//   redraws the slider;
// - a poll is dropped while anything is out or waiting;
// - a request that hangs is dropped after 8 s, longer than the server's own
//   5 s waits for a request and for each chunk it sends (HTTP_MAX_DATA_WAIT,
//   HTTP_MAX_SEND_WAIT, WebServer.h in arduino-esp32 2.0.17): our choice,
//   so the page never gives up on a reply the server may still finish. After
//   a drop the next request can overlap the dropped one's connection.
// tools/fx3d/page_queue_test.js runs this queue in JavaScriptCore.
//
// Served straight from flash with WebServer::send_P, which writes it out
// without a heap copy (arduino-esp32 2.0.17, libraries/WebServer/src/
// WebServer.cpp, send_P and sendContent_P).

#if defined(FX3D_ENABLED)

static const char kFx3dPage[] = R"FX3D(<!doctype html>
<html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>3D на панели</title>
<style>
:root{color-scheme:dark;--bg:#0d0f12;--card:#171a1f;--ink:#e8eaed;--dim:#9aa3ad;--on:#2d6cdf;--edge:#2a2f36}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.45 -apple-system,system-ui,sans-serif}
main{max-width:760px;margin:0 auto;padding:12px 16px 40px}h1{font-size:20px;margin:6px 0 2px}
.st{color:var(--dim);font-size:13px;margin:0 0 12px;min-height:1.4em}
section{background:var(--card);border-radius:10px;padding:12px 14px;margin:0 0 12px}
h2{font-size:15px;margin:0 0 8px}.g{display:flex;flex-wrap:wrap;gap:6px}
button{background:#232830;color:var(--ink);border:1px solid var(--edge);border-radius:8px;padding:8px 11px;font:inherit;cursor:pointer}
button.on{background:var(--on);border-color:var(--on)}label{display:flex;align-items:center;gap:10px;margin:8px 0 0;flex-wrap:wrap}
input[type=range]{flex:1;min-width:160px}.hint{color:var(--dim);font-size:13px;margin:8px 0 0}
.err{color:#ff8a80}
</style></head><body><main>
<h1>3D на панели</h1><p class="st" id="st">…</p>
<section><h2>Очки</h2><div class="g" id="modes"></div>
<label>Поменять глаза <button id="swap">нет</button></label>
<label>Глубина, пикселей <input type="range" id="depth" min="0" max="6" step="0.5"><b id="dv"></b></label>
<label>Левый глаз <input type="range" id="gl" min="0" max="100" step="5"><b id="glv"></b></label>
<label>Правый глаз <input type="range" id="gr" min="0" max="100" step="5"><b id="grv"></b></label>
<p class="hint">Начинайте с «красный–синий» и глубины 2: так советует бриф коллег. Больше глубины — только если глазам удобно.</p>
<p class="hint" id="kept"></p><div class="g"><button id="reset">Вернуть исходные</button></div></section>
<section><h2>Калибровка очков</h2><div class="g" id="cal"></div><p class="hint" id="calhint">Шесть шагов по порядку. Смотрите одним глазом, потом другим.</p></section>
<section><h2>Сцены</h2><div class="g" id="scenes"></div><p class="hint">Первое касание ручки возвращает панель на её страницу. * — тяжёлая: по замеру 18.09 меньше 30 кадров в секунду; в очках (анаглиф) каждая сцена стоит вдвое.</p></section>
<section><h2>Любой экран в 3D</h2><div class="g" id="looks"></div><p class="hint">Режим остаётся на всех страницах, пока не выбрано «как есть». Выпуклость, слои, парение и купол — для очков; покачивание, карточка, рельеф и барабан — без очков. Под режимом страница идёт медленнее: по замеру 18.09 это 10–20 кадров в секунду, карточка и рельеф — самые дорогие.</p></section>
<section><h2>Замер</h2><div class="g"><button id="bench">Замерить всё (≈4 мин)</button></div><p class="hint">Каждая сцена и режим по 5 секунд, результаты — в журнал панели. Пока идёт замер, остальное не переключается.</p></section>
</main><script>
const M={mono:"Без очков",redblue:"Красный–синий",redcyan:"Красный–голубой",redgreen:"Красный–зелёный"};
const S={calib:"Калибровка",cube:"Куб",layers:"Слои",stars:"Звёзды",helix:"Спираль",rings:"Кольца",dial:"Часы в слоях",torus:"Тор *",vclock:"Воксельные часы",voxel:"Полёт *",tunnel:"Туннель *",blobs:"Метаболы *",globe:"Глобус *",terrain:"Холмы звука * (звук ещё не подключён)"};
const L={flat:"Как есть",pop:"Выпуклость",layers:"Слои по цвету",float:"Парение",dome:"Купол",wiggle:"Покачивание",card:"Карточка",relief:"Рельеф",drum:"Барабан"};
const C=["Красный: каким глазом видно, каким гаснет?","Зелёный: то же самое","Синий: то же самое","Глаза: левому — черта и L, правому — черта и R. Чужая фигура видна — это утечка; L справа — поменяйте глаза","Плоскость панели: рамка и крест лежат на панели, пунктир — стык","Глубина: левый квадрат перед панелью, средний на ней, правый за ней"];
let s={},busy=false;const next=new Map(),$=i=>document.getElementById(i);
function q(k,f){if(k)next.set(k,f);else if(busy||next.size)return;else next.set("",()=>"");go()}
function go(){if(busy||!next.size)return;const[k,f]=next.entries().next().value;next.delete(k);const p=f();busy=true;
const c=new AbortController(),t=setTimeout(()=>c.abort(),8000);
fetch("/api/fx3d"+(p?"?"+p:""),{signal:c.signal}).then(r=>r.json().then(j=>({ok:r.ok,j}))).then(({ok,j})=>{if(!ok){$("st").textContent=j.error||"ошибка";$("st").className="st err";return}$("st").className="st";s=j;draw()}).catch(()=>{$("st").textContent="панель не отвечает";$("st").className="st err"}).then(()=>{clearTimeout(t);busy=false;go()})}
function btns(id,items,cur,cb){const g=$(id);g.innerHTML="";for(const[k,t]of items){const b=document.createElement("button");b.textContent=t;if(k===cur)b.className="on";b.onclick=()=>cb(k);g.appendChild(b)}}
function draw(){btns("modes",Object.entries(M),s.mode,k=>q("mode",()=>"mode="+k));
btns("scenes",[["off","Выключить"]].concat((s.scenes||[]).map(k=>[k,S[k]||k])),s.scene||"off",k=>q("scene",()=>"scene="+k));
btns("looks",(s.looks||[]).map(k=>[k,L[k]||k]),s.look,k=>q("look",()=>"look="+k));
btns("cal",C.map((t,i)=>[String(i),String(i+1)]),s.scene==="calib"?String(s.page):"",k=>{$("calhint").textContent=C[+k];q("scene",()=>"scene=calib&page="+k)});
$("swap").textContent=s.swap?"да":"нет";$("swap").className=s.swap?"on":"";
$("depth").value=s.depthPx;$("dv").textContent=s.depthPx;$("gl").value=s.gainL;$("glv").textContent=s.gainL+"%";$("gr").value=s.gainR;$("grv").textContent=s.gainR+"%";
$("kept").textContent=s.profile==="pending"?"Сохраню в панели через пару секунд (во время замера — после него).":s.profile==="failed"?"Панель не смогла сохранить настройки очков: после перезагрузки они могут оказаться другими. Попробую снова при следующем изменении.":"Настройки очков сохранены в панели и переживут перезагрузку.";$("kept").className=s.profile==="failed"?"hint err":"hint";
$("bench").className=s.bench?"on":"";$("bench").textContent=s.bench?"Замер идёт — остановить":"Замерить всё (≈4 мин)";
$("st").textContent=(s.scene?"сцена «"+(S[s.scene]||s.scene)+"»":"страница панели")+" · "+(L[s.look]||s.look)+" · "+(M[s.mode]||s.mode)+(s.fps>0?" · "+s.fps+" кадр/с, кадр "+s.frameUs+" мкс":"")}
$("swap").onclick=()=>q("swap",()=>"swap="+(s.swap?0:1));
for(const k of["depth","gl","gr"])$(k).onchange=e=>{const v=e.target.value;q(k,()=>k+"="+v)};
$("bench").onclick=()=>q("bench",()=>"bench="+(s.bench?0:1));
$("reset").onclick=()=>{if(confirm("Вернуть исходные настройки очков: красный–синий, глубина 2, глаза как есть, оба на 100 %?"))q("profile",()=>"profile=reset")};q();
setInterval(()=>{if(document.visibilityState==="visible")q()},3000);
</script></body></html>)FX3D";

#endif  // FX3D_ENABLED
