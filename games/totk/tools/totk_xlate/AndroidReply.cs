// Android's in-process shader path. Wire format and SPIR-V rewrites match android/tools/shader_bridge.py.
// No external process, Python interpreter or SPIRV-Cross is needed at runtime.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

static class AndroidReply
{
    sealed class Spv
    {
        internal List<int> W;
        internal Dictionary<int,string> Names=new();
        internal Dictionary<int,Dictionary<int,int>> Deco=new();
        internal Dictionary<int,(int op,int[] a)> Types=new();
        internal Dictionary<int,(int ptr,int storage)> Vars=new();
        internal Dictionary<int,int> Consts=new();
        internal HashSet<int> Caps=new();
        internal Spv(byte[] data)
        {
            if(data.Length<20 || data.Length%4!=0)throw new Exception("Invalid SPIR-V size");
            int[] words=new int[data.Length/4];Buffer.BlockCopy(data,0,words,0,data.Length);W=words.ToList();
            if(W[0]!=0x07230203)throw new Exception("Invalid SPIR-V magic");
            for(int i=5;i<W.Count;)
            {
                int op=W[i]&65535,n=(int)((uint)W[i]>>16);
                if(n==0 || i+n>W.Count)throw new Exception("Invalid SPIR-V instruction");
                int[] a=W.Skip(i).Take(n).ToArray();
                if(op==17)Caps.Add(a[1]);
                else if(op==5)Names[a[1]]=Str(a.Skip(2));
                else if(op==71){if(!Deco.ContainsKey(a[1]))Deco[a[1]]=new();Deco[a[1]][a[2]]=n>3?a[3]:1;}
                else if(new[]{19,20,21,22,23,24,25,26,27,28,29,30,32}.Contains(op))Types[a[1]]=(op,a.Skip(2).ToArray());
                else if(op==43)Consts[a[2]]=a[3];
                else if(op==59)Vars[a[2]]=(a[1],a[3]);
                i+=n;
            }
        }
        internal static string Str(IEnumerable<int> words){int[] a=words.ToArray();byte[] b=new byte[a.Length*4];Buffer.BlockCopy(a,0,b,0,b.Length);int n=Array.IndexOf(b,(byte)0);return Encoding.UTF8.GetString(b,0,n<0?b.Length:n);}
        internal byte[] Bytes(){int[] a=W.ToArray();byte[] b=new byte[a.Length*4];Buffer.BlockCopy(a,0,b,0,b.Length);return b;}
        internal int Dec(int v,int kind,int def=-1)=>Deco.TryGetValue(v,out var d)&&d.TryGetValue(kind,out int n)?n:def;
        internal bool Vec4(int t)=>Types.TryGetValue(t,out var a)&&a.op==23&&a.a[1]==4&&Types[a.a[0]].op==22&&Types[a.a[0]].a[0]==32;
        internal int Pointee(int v)=>Types[Vars[v].ptr].a[1];
        internal (int dtype,int count,int view) Descriptor(int v)
        {
            int t=Pointee(v),count=1;var a=Types[t];
            while(a.op==28 || a.op==29){count*=a.op==28?Consts.GetValueOrDefault(a.a[1],1):1;t=a.a[0];a=Types[t];}
            int View(int[] image){int dim=image[1],array=image[3],ms=image[4];int b=dim switch{0=>array!=0?4:0,1=>array!=0?5:1,2=>2,3=>array!=0?6:3,4 or 6=>array!=0?5:1,_=>-1};return b<0?-1:b+(ms!=0?256:0);}
            if(a.op==27){int[] image=Types[a.a[0]].a;return(image[1]==5?4:1,count,View(image));}
            if(a.op==26)return(0,count,-1);
            if(a.op==25)return(a.a[1]==5?(a.a[5]==2?5:4):(a.a[5]==2?3:2),count,a.a[1]==5?-1:View(a.a));
            if(a.op==30)return(Vars[v].storage==12||Dec(t,3)>=0?7:6,count,-1);
            throw new Exception("Unsupported descriptor type");
        }
        internal string Scalar(int t){while(Types[t].op is 23 or 28)t=Types[t].a[0];return Types[t].op==21?(Types[t].a[1]!=0?"sint":"uint"):"float";}
        internal List<(int location,string cls)> Interface(int storage)=>Vars.Where(x=>x.Value.storage==storage&&Dec(x.Key,30)>=0&&Dec(x.Key,11)<0).Select(x=>(Dec(x.Key,30),Scalar(Pointee(x.Key)))).OrderBy(x=>x.Item1).ToList();
        internal void OrderGlobals()
        {
            // Stable topological order of the SPIR-V type/constant/global section.
            // An unknown instruction leaves the section untouched.
            var layout=new Dictionary<int,(int result,int[] ids,int tail)>();
            foreach(int op in new[]{19,20,21,22,26,31})layout[op]=(1,Array.Empty<int>(),-1);
            foreach(int op in new[]{23,24,25,27,29})layout[op]=(1,new[]{2},-1);
            layout[28]=(1,new[]{2,3},-1);layout[30]=(1,Array.Empty<int>(),2);layout[32]=(1,new[]{3},-1);layout[33]=(1,Array.Empty<int>(),2);
            foreach(int op in new[]{41,42,43,45,46,48,49,50,1})layout[op]=(2,new[]{1},-1);
            layout[44]=(2,new[]{1},3);layout[51]=(2,new[]{1},3);layout[52]=(2,new[]{1},4);layout[59]=(2,new[]{1,4},-1);
            var inst=new List<int[]>();for(int i=5;i<W.Count;){int n=(int)((uint)W[i]>>16);inst.Add(W.Skip(i).Take(n).ToArray());i+=n;}
            int first=inst.FindIndex(a=>layout.ContainsKey(a[0]&65535)),end=inst.FindIndex(a=>(a[0]&65535)==54);if(end<0)end=inst.Count;
            if(first<0||first>=end)return;var section=inst.Skip(first).Take(end-first).ToArray();
            if(section.Any(a=>!layout.ContainsKey(a[0]&65535)))return;
            var defined=new Dictionary<int,int>();var deps=new List<int[]>();
            for(int k=0;k<section.Length;k++){var a=section[k];var l=layout[a[0]&65535];defined[a[l.result]]=k;deps.Add(l.ids.Where(i=>i<a.Length).Select(i=>a[i]).Concat(l.tail<0?Array.Empty<int>():a.Skip(l.tail)).ToArray());}
            if(!deps.Select((d,k)=>d.Any(x=>defined.GetValueOrDefault(x,-1)>k)).Any(x=>x))return;
            var users=section.Select(_=>new List<int>()).ToArray();var need=new int[section.Length];
            for(int k=0;k<deps.Count;k++)foreach(int x in deps[k].Distinct())if(defined.TryGetValue(x,out int j)&&j!=k){users[j].Add(k);need[k]++;}
            var ready=new SortedSet<int>(Enumerable.Range(0,section.Length).Where(k=>need[k]==0));var order=new List<int>();
            while(ready.Count>0){int k=ready.Min;ready.Remove(k);order.Add(k);foreach(int u in users[k])if(--need[u]==0)ready.Add(u);}
            if(order.Count!=section.Length)throw new Exception("SPIR-V global dependency cycle");
            W=W.Take(5).Concat(inst.Take(first).SelectMany(a=>a)).Concat(order.SelectMany(k=>section[k])).Concat(inst.Skip(end).SelectMany(a=>a)).ToList();
        }
        internal void StripInterlock()
        {
            List<int> o=W.Take(5).ToList();
            for(int i=5;i<W.Count;){int n=(int)((uint)W[i]>>16),op=W[i]&65535;var a=W.Skip(i).Take(n).ToArray();
                bool drop=(op==17&&new[]{5363,5372,5378}.Contains(a[1]))||op is 5364 or 5365||(op==16&&a[2]>=5366&&a[2]<=5371)||(op==10&&Str(a.Skip(1))=="SPV_EXT_fragment_shader_interlock");
                if(!drop)o.AddRange(a);i+=n;
            }W=o;
        }
        internal void Set(int set){for(int i=5;i<W.Count;){int n=(int)((uint)W[i]>>16);if((W[i]&65535)==71&&W[i+2]==34)W[i+3]=set;i+=n;}}
        internal void Defaults()
        {
            var outs=Vars.Where(x=>x.Value.storage==3&&Dec(x.Key,30)>=0&&Dec(x.Key,11)<0&&Vec4(Pointee(x.Key))).Select(x=>x.Key).ToList();
            int entry=-1;for(int i=5;i<W.Count;){if((W[i]&65535)==15){entry=W[i+2];break;}i+=(int)((uint)W[i]>>16);}
            if(outs.Count==0||entry<0)return;
            List<int> decls=new();Dictionary<int,int> cs=new();int bound=W[3];
            foreach(int v in outs){int t=Pointee(v);if(cs.ContainsKey(t))continue;int f=Types[t].a[0],z=bound++,one=bound++,vec=bound++;cs[t]=vec;decls.AddRange(new[]{4<<16|43,f,z,0,4<<16|43,f,one,0x3f800000,7<<16|44,t,vec,z,z,z,one});}
            var o=W.Take(5).ToList();int state=0;
            for(int i=5;i<W.Count;){int n=(int)((uint)W[i]>>16),op=W[i]&65535;var a=W.Skip(i).Take(n).ToArray();
                if(op==54&&decls.Count>0){o.AddRange(decls);decls.Clear();}
                if(state==0&&op==54&&a[2]==entry)state=1;
                else if(state==1&&op==248){o.AddRange(a);i+=n;while(i<W.Count&&(W[i]&65535)==59){int k=(int)((uint)W[i]>>16);o.AddRange(W.Skip(i).Take(k));i+=k;}foreach(int v in outs)o.AddRange(new[]{3<<16|62,v,cs[Pointee(v)]});state=2;continue;}
                o.AddRange(a);i+=n;
            }
            if(state!=2)throw new Exception("No vertex entry block");o[3]=bound;W=o;
        }
        internal void GatherNudge()
        {
            int Find(int op,int[] a)=>Types.Where(x=>x.Value.op==op&&x.Value.a.SequenceEqual(a)).Select(x=>x.Key).DefaultIfEmpty(-1).First();
            int f32=Find(22,new[]{32});if(f32<0)return;
            Dictionary<int,int> result=new(),adds=new(),divs=new();HashSet<int> nudges=new();List<int> gathers=new();
            for(int i=5;i<W.Count;){int n=(int)((uint)W[i]>>16),op=W[i]&65535;
                if(new[]{61,86,83,245,169}.Contains(op)&&n>2)result[W[i+2]]=W[i+1];
                else if(op==43&&n==4&&W[i+3]==0x3b000000)nudges.Add(W[i+2]);
                else if(op==44&&n>=5&&nudges.Contains(W[i+3])&&nudges.Contains(W[i+4]))nudges.Add(W[i+2]);
                else if(op==129)adds[W[i+2]]=W[i+4];else if(op==136)divs[W[i+2]]=W[i+3];else if(op==96)gathers.Add(i);i+=n;
            }
            Dictionary<int,(int image,int dims)> plan=new();
            foreach(int at in gathers){if(divs.TryGetValue(adds.GetValueOrDefault(W[at+4],-1),out int off)&&nudges.Contains(off))continue;
                if(!Types.TryGetValue(result.GetValueOrDefault(W[at+3],-1),out var t)||t.op!=27)continue;
                var image=Types[t.a[0]];if(image.op!=25||image.a[1]!=1||image.a[4]!=0)continue;plan[at]=(t.a[0],image.a[3]!=0?3:2);
            }if(plan.Count==0)return;
            int bound=W[3];List<int> decls=new();
            int Declare(int op,int[] a){int t=Find(op,a);if(t<0){t=bound++;decls.Add((a.Length+2)<<16|op);decls.Add(t);decls.AddRange(a);Types[t]=(op,a);}return t;}
            int i32=Declare(21,new[]{32,1}),zi=bound++,zf=bound++,nu=bound++;
            decls.AddRange(new[]{4<<16|43,i32,zi,0,4<<16|43,f32,zf,0,4<<16|43,f32,nu,0x3b000000});
            Dictionary<int,(int fv,int iv,int c)> vec=new();
            foreach(int dims in plan.Values.Select(x=>x.dims).Distinct().OrderBy(x=>x)){int fv=Declare(23,new[]{f32,dims}),iv=Declare(23,new[]{i32,dims}),c=bound++;decls.AddRange(new[]{(3+dims)<<16|44,fv,c,nu,nu});if(dims==3)decls.Add(zf);vec[dims]=(fv,iv,c);}
            List<int> o=W.Take(5).ToList();bool declared=false,caps=Caps.Contains(50);
            for(int i=5;i<W.Count;){int n=(int)((uint)W[i]>>16),op=W[i]&65535;int[] a=W.Skip(i).Take(n).ToArray();
                if(!caps&&op!=17){o.AddRange(new[]{2<<16|17,50});caps=true;}
                if(op==54&&!declared){o.AddRange(decls);declared=true;}
                if(plan.TryGetValue(i,out var p)){var v=vec[p.dims];int img=bound++,size=bound++,sizef=bound++,off=bound++,coord=bound++;
                    o.AddRange(new[]{4<<16|100,p.image,img,a[3],5<<16|103,v.iv,size,img,zi,4<<16|111,v.fv,sizef,size,5<<16|136,v.fv,off,v.c,sizef,5<<16|129,v.fv,coord,a[4],off});a[4]=coord;
                }o.AddRange(a);i+=n;
            }o[3]=bound;W=o;
        }
    }
    static int N(JsonNode n)=>n==null?0:n.GetValue<int>();
    static (int? t,int? s,int ty) Decode(string name)
    {
        var m=Regex.Match(name,@"^[a-z]+_[ti]\w*?_(?:cb(\d+)|tcb)_([0-9A-F]+)");if(!m.Success)return(null,null,0);
        long slots=m.Groups[1].Success?long.Parse(m.Groups[1].Value):-1,h=Convert.ToInt64(m.Groups[2].Value,16);int tb=slots>=0?(int)(slots&65535):2,sb=slots>=0&&(slots>>16)!=0?(int)(slots>>16)-1:tb,ty=(int)(h>>28);
        int[] words=ty!=0?new[]{(int)(h&0x3fff),(int)((h>>14)&0x3fff)}:new[]{(int)(h&0x3fff)};
        int? Slot(int x,int b,int cb)=>cb==2&&x*4>=b&&(x*4-b)%8==0?(x*4-b)/8:null;
        int? First(Func<int,int?> f){foreach(int x in words){var v=f(x);if(v.HasValue)return v;}return null;}
        if(Regex.IsMatch(name,@"^[a-z]+_i"))return(First(x=>x*4>=0x120&&x*4<0x168?Slot(x,0x120,tb):null),null,ty);
        int? t=First(x=>x*4<0x568?Slot(x,0x168,tb):null)??First(x=>x*4>=0x20&&x*4<0x120?Slot(x,0x20,tb):null);
        return(t,First(x=>Slot(x,0x568,sb)),ty);
    }
    static List<string> Bindings(string stage,Spv spv,JsonObject res)
    {
        var rows=new List<string>();var sbufs=res["sbufs"].AsArray().OrderBy(x=>N(x["binding"])).ToArray();int si=0,baseOff=stage=="vs"?0x110:stage=="cs"?0x310:0x510;
        foreach(var item in spv.Vars.Where(x=>x.Value.storage is 0 or 2 or 12 &&spv.Dec(x.Key,33)>=0).OrderBy(x=>spv.Dec(x.Key,33)).ThenBy(x=>x.Key))
        {
            int v=item.Key,binding=spv.Dec(v,33),kind=0,reg=-1,sreg=-1,off=0,rw=0;var d=spv.Descriptor(v);string name=spv.Names.GetValueOrDefault(v,"");var cb=Regex.Match(name,@"^[a-z]+_c(\d+)$");
            if(name=="support_buffer")kind=7;
            else if(cb.Success){int n=int.Parse(cb.Groups[1].Value);if(n>=3&&n<=16){kind=1;reg=n-3;}else kind=n switch{1=>8,0=>9,2=>10,_=>throw new Exception("Unsupported constant bank")};}
            else if(Regex.IsMatch(name,@"^[a-z]+_s\d+$")){
                JsonNode b=si<sbufs.Length?sbufs[si]:null;si++;int slot=N(b?["cb_slot"]);int? offset=b!=null&&slot==0?N(b["cb_offset"])*4:null;rw=b!=null&&(N(b["flags"])&1)!=0?1:0;
                if(stage=="cs"&&offset>=0x210&&offset<0x310&&(offset-0x210)%16==0){kind=12;reg=(offset.Value-0x210)/16;}
                else if(b!=null&&slot>=3&&slot<=16){kind=13;reg=slot-3;off=N(b["cb_offset"])*4;}
                else if(offset.HasValue&&offset-baseOff>=0&&offset-baseOff<0x100&&(offset-baseOff)%16==0){kind=6;reg=(offset.Value-baseOff)/16;}
                else throw new Exception("Storage address not mapped: "+name);
            }else if(Regex.IsMatch(name,@"^[a-z]+_t\w*?_cb\d+_40000000$") && d.dtype is 4 or 5){kind=5;reg=-2;}
            else if(Regex.IsMatch(name,@"^[a-z]+_t\w*?_cb\d+_40000000$") && d.dtype==2){kind=3;reg=-2;}
            else if(Regex.IsMatch(name,@"^[a-z]+_s\w*?_cb\d+_40000000$") && d.dtype==0){kind=11;reg=-2;}
            else{var t=Decode(name);if(!t.t.HasValue)throw new Exception("Resource has no NVN slot: "+name);reg=t.t.Value;if(Regex.IsMatch(name,@"^[a-z]+_i"))kind=4;else if(d.dtype is 4 or 5)kind=5;else{kind=3;sreg=t.ty!=0?t.s??-1:-1;}}
            rows.Add($"B {(stage=="ps"?1:0)} {binding} {kind} {reg} {sreg} {off} {rw} {d.dtype} {d.count} {d.view}");
        }return rows;
    }
    internal static byte[] Translate(string request,string work)
    {
        var rq=JsonNode.Parse(request).AsObject();string variant=(string)rq["variant"],stage=(string)rq["stage"];
        if(!Regex.IsMatch(variant??"",@"\A[0-9a-f]{32}-[0-9a-f]{12}\z")||stage is not ("vs" or "ps" or "cs"))throw new Exception("Invalid shader request");
        byte[] code=Convert.FromHexString((string)rq["code_hex"]);if(code.Length==0||code.Length>16<<20)throw new Exception("Invalid shader size");
        Directory.CreateDirectory(work);string input=Path.Combine(work,variant+".code");File.WriteAllBytes(input,code);
        var job=new JsonObject{["key"]=variant,["stage"]=stage,["code"]=input,["c1"]=(string)rq["c1"]??"",["debug"]=true,["reduced"]=true,["depth_minus_w"]=rq["depth_minus_w"]?.DeepClone()??JsonValue.Create(false)};
        foreach(string k in new[]{"tex","img","attr","fout"})job[k]=rq[k]?.DeepClone()??new JsonObject();if(rq["local"]!=null)job["local"]=rq["local"].DeepClone();
        try{
            string err=SwshXlate.Translate(job,work);if(err!=null)throw new Exception(err);
            string spath=Path.Combine(work,variant+".spv"),rpath=Path.Combine(work,variant+".res.json");
            var spv=new Spv(File.ReadAllBytes(spath));spv.OrderGlobals();
            if(spv.Caps.Contains(10)){job["softfloat64"]=true;err=SwshXlate.Translate(job,work);if(err!=null)throw new Exception(err);spv=new Spv(File.ReadAllBytes(spath));spv.OrderGlobals();}
            if(spv.Caps.Contains(10)||spv.Caps.Contains(11))throw new Exception("Unsupported 64-bit shader capability");
            var rows=Bindings(stage,spv,JsonNode.Parse(File.ReadAllText(rpath)).AsObject());
            spv.StripInterlock();spv=new Spv(spv.Bytes());spv.Set(stage=="ps"?1:0);if(stage=="vs")spv.Defaults();spv=new Spv(spv.Bytes());
#if TOTK_ANDROID_GATHER_NUDGE
            spv.GatherNudge();
#endif
            byte[] data=spv.Bytes();var ins=stage=="cs"?new List<(int location,string cls)>():spv.Interface(1);var outs=stage=="cs"?new List<(int location,string cls)>():spv.Interface(3);var local=rq["local"]?.AsArray();
            var head=new List<string>{$"OK {data.Length} {rows.Count} {ins.Count} {outs.Count} {N(local?[0])} {N(local?[1])} {N(local?[2])}"};head.AddRange(rows);head.AddRange(ins.Select(x=>$"I {x.location} {x.cls}"));head.AddRange(outs.Select(x=>$"O {x.location} {x.cls}"));
            byte[] text=Encoding.UTF8.GetBytes(string.Join("\n",head)+"\n");return text.Concat(data).ToArray();
        }finally{foreach(string ext in new[]{".code",".spv",".res.json"}){string p=Path.Combine(work,variant+ext);if(File.Exists(p))File.Delete(p);}}
    }
    [UnmanagedCallersOnly(EntryPoint="totk_shader_translate")]
    internal static unsafe byte* NativeTranslate(byte* request,nuint length,byte* directory,nuint* outputLength)
    {
        try{if(length>64u<<20)throw new Exception("Oversize shader request");byte[] result=Translate(Encoding.UTF8.GetString(new ReadOnlySpan<byte>(request,checked((int)length))),Marshal.PtrToStringUTF8((IntPtr)directory));byte* p=(byte*)NativeMemory.Alloc((nuint)result.Length);result.CopyTo(new Span<byte>(p,result.Length));*outputLength=(nuint)result.Length;return p;}
        catch(Exception e){byte[] result=Encoding.UTF8.GetBytes("ERR "+e.Message.Replace('\n',' ')+"\n");byte* p=(byte*)NativeMemory.Alloc((nuint)result.Length);result.CopyTo(new Span<byte>(p,result.Length));*outputLength=(nuint)result.Length;return p;}
    }
    [UnmanagedCallersOnly(EntryPoint="totk_shader_free")]
    internal static unsafe void NativeFree(void* p)=>NativeMemory.Free(p);
}
