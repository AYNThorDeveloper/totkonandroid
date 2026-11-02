#!/usr/bin/env python3
"""Capture only the project-authored settings HTML, with illustrative bridge values."""
import argparse,json,os,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--chrome',default=os.environ.get('CHROME','/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'))
    a=p.parse_args()
    za=(ROOT/'games/totk/assets/expected-za-2.0.2.json').exists()
    config={'fps':60 if za else 0,'render':[1920,1080] if za else [1280,720],'output':[1920,1080],'activeOutput':[1920,1080],'lang':''}
    mock='window.VideoSettings={read:()=>JSON.stringify('+json.dumps(config)+'),overlay:()=>true,language:()=>"",save:()=>true,setLanguage:()=>true,close:()=>{}};'
    html=(ROOT/'android/game_activity/assets/video-settings.html').read_text()
    html=html.replace('<script>','<script>'+mock+'\n',1)
    annotation='<script>window.videoOpen();const n=document.createElement("div");n.textContent="Project UI preview · illustrative settings · no game content";n.style="position:fixed;bottom:5px;right:16px;font:11px Arial;color:#b8c1ca;z-index:999";document.body.append(n);</script>'
    html=html.replace('</body>',annotation+'</body>')
    out=ROOT/'docs/images/video-settings.png';out.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='settings-ui-preview-') as work:
        page=Path(work)/'preview.html';page.write_text(html)
        subprocess.run([a.chrome,'--headless=new','--disable-gpu','--disable-background-networking','--no-first-run','--hide-scrollbars','--force-device-scale-factor=1','--window-size=1440,900','--virtual-time-budget=1500','--user-data-dir='+str(Path(work)/'profile'),'--screenshot='+str(out),page.as_uri()],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    print(out)
if __name__=='__main__':main()
