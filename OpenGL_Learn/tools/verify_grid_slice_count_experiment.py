#!/usr/bin/env python3
"""Independent CSR, image, truth, compatibility, and cache verifier."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import subprocess
from pathlib import Path
from typing import Any

import matplotlib
matplotlib.use("Agg")
import numpy as np
from PIL import Image

TILE=16
SLICES=(1,2,4,8,16)
COUNTS=(32,64,128,256,512)
RADII=(1.5,3.0,6.0,8.0,12.0)
NEAR=np.float32(0.1)
FAR=np.float32(100.0)


def load(path:Path)->dict[str,Any]:return json.loads(path.read_text(encoding="utf-8-sig"))
def dump(path:Path,value:Any)->None:
    path.parent.mkdir(parents=True,exist_ok=True);path.write_text(json.dumps(value,ensure_ascii=False,indent=2,allow_nan=False)+"\n",encoding="utf-8")
def sha(path:Path)->str:
    digest=hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda:stream.read(1024*1024),b""):digest.update(block)
    return digest.hexdigest().upper()


def read_pfm(path:Path)->np.ndarray:
    with path.open("rb") as stream:
        magic=stream.readline().strip();width,height=(int(v) for v in stream.readline().split());scale=float(stream.readline());channels=3 if magic==b"PF" else 1
        if magic not in (b"PF",b"Pf"):raise ValueError(f"bad PFM {path}")
        values=np.fromfile(stream,dtype="<f4" if scale<0 else ">f4")
    shape=(height,width,channels) if channels==3 else (height,width)
    if values.size!=int(np.prod(shape)):raise ValueError(f"PFM payload mismatch {path}")
    result=values.reshape(shape)
    if not np.all(np.isfinite(result)):raise ValueError(f"nonfinite PFM {path}")
    return result


def build_helper(run_dir:Path)->Path:
    source=Path(__file__).with_name("fnv1a64_stdin.cpp").resolve();directory=run_dir/"verification";directory.mkdir(parents=True,exist_ok=True)
    exe=directory/"fnv1a64_stdin.exe";stamp=directory/"fnv1a64_stdin.source.sha256";source_hash=sha(source)
    if exe.exists() and stamp.exists() and stamp.read_text(encoding="ascii").strip()==source_hash:return exe
    vcvars=sorted(Path(r"C:\Program Files\Microsoft Visual Studio\2022").glob("*/VC/Auxiliary/Build/vcvars64.bat"))
    if not vcvars:raise RuntimeError("vcvars64.bat not found")
    obj=directory/"fnv1a64_stdin.obj";command=f'call "{vcvars[0]}" >nul && cl /nologo /O2 /EHsc "{source}" /Fo:"{obj}" /Fe:"{exe}"'
    result=subprocess.run(command,cwd=directory,shell=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
    if result.returncode or not exe.exists():raise RuntimeError(result.stdout)
    stamp.write_text(source_hash+"\n",encoding="ascii");return exe


def fnv(helper:Path,*arrays:np.ndarray)->str:
    process=subprocess.Popen([str(helper)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE);assert process.stdin
    try:
        for array in arrays:
            payload=memoryview(np.ascontiguousarray(array)).cast("B")
            for begin in range(0,len(payload),4*1024*1024):process.stdin.write(payload[begin:begin+4*1024*1024])
    finally:process.stdin.close()
    assert process.stdout and process.stderr
    output=process.stdout.read().decode("ascii").strip();error=process.stderr.read().decode("utf-8",errors="replace");code=process.wait()
    if code or not re.fullmatch(r"0x[0-9a-f]{16}",output):raise RuntimeError(f"FNV failed {code}: {error} {output}")
    return output


def normalize(vectors:np.ndarray)->np.ndarray:
    lengths=np.sqrt(np.sum(vectors*vectors,axis=1,dtype=np.float32)).astype(np.float32);return vectors/lengths[:,None]


def planes(width:int,height:int,projection:np.ndarray)->tuple[np.ndarray,...]:
    tx=(width+TILE-1)//TILE;ty=(height+TILE-1)//TILE;arrays=[np.empty((tx*ty,3),dtype=np.float32) for _ in range(4)];cursor=0;fw=np.float32(width);fh=np.float32(height)
    for y in range(ty):
        y0=y*TILE;y1=min(height,y0+TILE);ny0=np.float32(np.float32(2)*np.float32(y0)/fh-np.float32(1));ny1=np.float32(np.float32(2)*np.float32(y1)/fh-np.float32(1));c=np.float32(ny0/projection[1,1]);d=np.float32(ny1/projection[1,1])
        for x in range(tx):
            x0=x*TILE;x1=min(width,x0+TILE);nx0=np.float32(np.float32(2)*np.float32(x0)/fw-np.float32(1));nx1=np.float32(np.float32(2)*np.float32(x1)/fw-np.float32(1));a=np.float32(nx0/projection[0,0]);b=np.float32(nx1/projection[0,0])
            arrays[0][cursor]=(1,0,a);arrays[1][cursor]=(-1,0,-b);arrays[2][cursor]=(0,1,c);arrays[3][cursor]=(0,-1,-d);cursor+=1
    return tuple(normalize(array) for array in arrays)


def depth_slice(depth:float,s:int)->int:
    if s<=1:return 0
    clamped=np.float32(max(float(NEAR),min(float(FAR),depth)))
    normalized=np.float32(np.log(np.float32(clamped/NEAR))/np.log(np.float32(FAR/NEAR)))
    return max(0,min(s-1,int(math.floor(float(np.float32(normalized*np.float32(s)))))))


def base_membership(result:dict[str,Any])->tuple[np.ndarray,list[tuple[float,float]],int,int]:
    point=result["pointLightStress"];width,height=(int(v) for v in result["resolution"]);view=np.asarray(result["gBuffer"]["cameraMatrices"]["view"],dtype=np.float32);projection=np.asarray(result["gBuffer"]["cameraMatrices"]["projection"],dtype=np.float32)
    positions=np.asarray([light["position"] for light in point["lights"]],dtype=np.float32);count=len(positions);tx=(width+TILE-1)//TILE;ty=(height+TILE-1)//TILE;tile_count=tx*ty
    homogeneous=np.concatenate((positions,np.ones((count,1),dtype=np.float32)),axis=1) if count else np.empty((0,4),dtype=np.float32)
    centers=(view@homogeneous.T).T[:,:3].astype(np.float32) if count else np.empty((0,3),dtype=np.float32);guarded=np.float32(np.float32(point["volumeRadius"])+np.float32(1e-6));membership=np.zeros((tile_count,count),dtype=bool);depth_ranges=[]
    side_planes=planes(width,height,projection)
    for index,center in enumerate(centers):
        depth=float(-center[2]);minimum=depth-float(guarded);maximum=depth+float(guarded)
        if not math.isfinite(depth) or maximum<float(NEAR) or minimum>float(FAR):depth_ranges.append((1.0,0.0));continue
        member=np.ones(tile_count,dtype=bool)
        for normal in side_planes:member&=(normal@center)>=-guarded
        membership[:,index]=member;depth_ranges.append((max(float(NEAR),minimum),min(float(FAR),maximum)))
    return membership,depth_ranges,tx,ty


def encode(base:np.ndarray,ranges:list[tuple[float,float]],s:int,helper:Path)->dict[str,Any]:
    tile_count,light_count=base.shape
    if s==1:membership=base
    else:
        membership=np.zeros((s,tile_count,light_count),dtype=bool)
        for index,(minimum,maximum) in enumerate(ranges):
            if minimum<=maximum:
                lo=depth_slice(minimum,s);hi=depth_slice(maximum,s);membership[lo:hi+1,:,index]=base[:,index]
        membership=membership.reshape(s*tile_count,light_count)
    counts=np.sum(membership,axis=1,dtype=np.uint32);prefix=np.empty(counts.size,dtype=np.uint32)
    if counts.size:
        cumulative=np.cumsum(counts,dtype=np.uint64)
        if int(cumulative[-1])>np.iinfo(np.uint32).max:raise OverflowError("independent CSR uint32 overflow")
        prefix[0]=0
        if counts.size>1:prefix[1:]=cumulative[:-1].astype(np.uint32)
    metadata=np.column_stack((prefix,counts)).astype("<u4",copy=False);indices=np.nonzero(membership)[1].astype("<u4",copy=False);light_bytes=light_count*4*16
    return {"logicalCells":int(counts.size),"nonEmptyCells":int(np.count_nonzero(counts)),"totalIndices":int(indices.size),"maximumLightsPerCell":int(np.max(counts)) if counts.size else 0,
            "averageLightsPerCell":float(np.mean(counts)) if counts.size else 0.0,"metadataBytes":int(metadata.nbytes),"indexBytes":int(indices.nbytes),"lightBytes":light_bytes,
            "residentBytes":int(metadata.nbytes+indices.nbytes+light_bytes),"csrSignature":fnv(helper,metadata,indices),"membership":membership}


def compare(encoded:dict[str,Any],runtime:dict[str,Any],label:str)->dict[str,bool]:
    fields=("logicalCells","nonEmptyCells","totalIndices","maximumLightsPerCell","metadataBytes","indexBytes","lightBytes","residentBytes")
    checks={field:int(encoded[field])==int(runtime[field]) for field in fields};checks["averageLightsPerCell"]=abs(encoded["averageLightsPerCell"]-float(runtime["averageLightsPerCell"]))<=1e-9;checks["csrSignature"]=encoded["csrSignature"].lower()==str(runtime["csrSignature"]).lower()
    failed=[field for field,value in checks.items() if not value]
    if failed:raise RuntimeError(f"CSR mismatch {label}: {[(field,encoded.get(field),runtime.get(field)) for field in failed]}")
    return checks


def quality(oracle:Path,candidate:Path)->dict[str,Any]:
    a=np.asarray(Image.open(oracle).convert("RGB"),dtype=np.int16);b=np.asarray(Image.open(candidate).convert("RGB"),dtype=np.int16);difference=np.abs(a-b);mse=float(np.mean((a.astype(np.float64)-b.astype(np.float64))**2));psnr=float("inf") if mse==0 else 10*math.log10(255**2/mse)
    return {"maxChannelLsb":int(np.max(difference)),"meanChannelLsb":float(np.mean(difference)),"p99ChannelLsb":float(np.percentile(difference,99)),"differentPixelCount":int(np.count_nonzero(np.any(difference!=0,axis=2))),"psnrDb":psnr,
            "passedFrozenGate":bool(np.max(difference)<=2 and np.mean(difference)<=.1 and np.percentile(difference,99)<=1),"difference":difference}


def truth(position:np.ndarray,validity:np.ndarray,result:dict[str,Any],encoded:dict[int,dict[str,Any]],tx:int,ty:int)->dict[str,Any]:
    height,width,_=position.shape;flat=np.flatnonzero(validity.reshape(-1));world=position.reshape(-1,3)[flat].astype(np.float32);yy=flat//width;xx=flat%width;tile_ids=(yy//TILE)*tx+(xx//TILE)
    view=np.asarray(result["gBuffer"]["cameraMatrices"]["view"],dtype=np.float32);homogeneous=np.concatenate((world,np.ones((len(world),1),dtype=np.float32)),axis=1);depths=-((view@homogeneous.T).T[:,2]);lights=np.asarray([item["position"] for item in result["pointLightStress"]["lights"]],dtype=np.float32);radius2=np.float32(float(result["pointLightStress"]["volumeRadius"])**2)
    cell_ids={1:tile_ids}
    for s in SLICES[1:]:
        normalized=np.log(np.clip(depths,NEAR,FAR)/NEAR)/np.log(FAR/NEAR);slice_ids=np.clip(np.floor(normalized*np.float32(s)).astype(np.int32),0,s-1);cell_ids[s]=slice_ids*(tx*ty)+tile_ids
    misses={s:0 for s in SLICES};interactions=0
    for light_index,light in enumerate(lights):
        delta=world-light;inside=np.einsum("ij,ij->i",delta,delta)<=radius2;interactions+=int(np.count_nonzero(inside))
        if np.any(inside):
            for s in SLICES:misses[s]+=int(np.count_nonzero(~encoded[s]["membership"][cell_ids[s][inside],light_index]))
    if any(misses.values()):raise RuntimeError(f"truth misses: {misses}")
    return {"validPixelCount":int(len(flat)),"invalidPixelCount":int(width*height-len(flat)),"groundTruthInteractions":interactions,"missesBySlice":{str(k):v for k,v in misses.items()}}


def main()->int:
    parser=argparse.ArgumentParser();parser.add_argument("--run-dir",type=Path,required=True);args=parser.parse_args();run_dir=args.run_dir.resolve();project=Path(__file__).resolve().parent.parent
    pre=load(run_dir/"pre-capture-manifest.json");manifest=load(run_dir/"capture-manifest.json");correctness=load(run_dir/"correctness-manifest.json");protocol=sha(run_dir/pre["protocol"])
    if protocol!=pre["protocolSha256"] or protocol!=manifest["protocolSha256"] or protocol!=correctness["protocolSha256"]:raise ValueError("protocol chain mismatch")
    helper=build_helper(run_dir);expected={item["stem"]:item for item in pre["expectedRuns"]};representative={}
    for item in expected.values():
        if item["regime"]=="cached" and int(item["round"])==1:representative[(int(item["lightCount"]),float(item["radius"]),int(item["sliceCount"]))]=load(run_dir/item["result"])
    csr=[];truth_cases={};csr_count=0
    for n in COUNTS:
        for r in RADII:
            source=representative[(n,r,1)];base,ranges,tx,ty=base_membership(source);entry={"lightCount":n,"radius":r,"submissionSignature":source["pointLightStress"]["submissionSignature"],"slices":{}}
            retained={}
            for s in SLICES:
                result=representative[(n,r,s)]
                if result["pointLightStress"]["submissionSignature"]!=entry["submissionSignature"]:raise ValueError(f"submission drift N{n}/R{r}/S{s}")
                encoded=encode(base,ranges,s,helper);checks=compare(encoded,result["pointLightStress"]["gridRuntime"],f"N{n}/R{r}/S{s}");entry["slices"][str(s)]={"values":{key:value for key,value in encoded.items() if key!="membership"},"checks":checks};csr_count+=1
                if (n,r) in ((512,1.5),(256,6.0),(512,12.0)):retained[s]=encoded
                else:del encoded
            csr.append(entry)
            if retained:truth_cases[(n,r)]=(source,retained,tx,ty)
    ce={item["stem"]:item for item in correctness["expectedRuns"]};cd={item["stem"]:item for item in correctness["completedRuns"]}
    for stem,item in ce.items():
        done=cd[stem]
        if sha(run_dir/item["result"])!=done["resultSha256"] or sha(run_dir/item["capture"])!=done["captureSha256"] or sha(run_dir/item["log"])!=done["logSha256"]:raise ValueError(f"correctness hash {stem}")
    chart=run_dir/"charts"/"correctness";chart.mkdir(parents=True,exist_ok=True);quality_cases=[]
    for name in ("low-n0512-r015","boundary-n0256-r060","high-n0512-r120"):
        oracle=run_dir/ce[f"quality-{name}-analytic"]["capture"];grid_paths=[run_dir/ce[f"quality-{name}-s{s:02d}"]["capture"] for s in SLICES]
        if len({sha(path) for path in grid_paths})!=1:raise RuntimeError(f"grid image mismatch {name}")
        q=quality(oracle,grid_paths[0]);difference=q.pop("difference")
        if not q["passedFrozenGate"]:raise RuntimeError(f"Oracle gate {name}: {q}")
        Image.fromarray(np.clip(difference*96,0,255).astype(np.uint8),mode="RGB").save(chart/f"{name}-difference-x96.png");quality_cases.append({"case":name,"allGridExact":True,**q})
    edges=[]
    for name in ("n0","n1","fixtures"):
        paths=[run_dir/ce[f"edge-{name}-s{s:02d}"]["capture"] for s in SLICES]
        if len({sha(path) for path in paths})!=1:raise RuntimeError(f"edge mismatch {name}")
        result=load(run_dir/ce[f"edge-{name}-s16"]["result"]);fixtures=result["pointLightStress"]["fixtures"]
        if name=="fixtures" and not all(bool(fixtures[key]) for key in ("nearPlaneIntersectionVerified","cameraInsideVerified","fullyOffscreenVerified","depthSliceBoundaryPlaced")):raise RuntimeError(f"fixture failure {fixtures}")
        edges.append({"case":name,"allSlicesExact":True,"fixtures":fixtures})
    tile_legacy=load(run_dir/ce["compat-tile-legacy"]["result"]);tile_explicit=load(run_dir/ce["compat-tile-explicit-s1"]["result"]);cluster_legacy=load(run_dir/ce["compat-cluster-legacy"]["result"]);cluster_explicit=load(run_dir/ce["compat-cluster-explicit-s16"]["result"])
    compat={"tileImageExact":sha(run_dir/ce["compat-tile-legacy"]["capture"])==sha(run_dir/ce["compat-tile-explicit-s1"]["capture"]),"tileCsrExact":tile_legacy["pointLightStress"]["gridRuntime"]["csrSignature"]==tile_explicit["pointLightStress"]["gridRuntime"]["csrSignature"],
            "clusterImageExact":sha(run_dir/ce["compat-cluster-legacy"]["capture"])==sha(run_dir/ce["compat-cluster-explicit-s16"]["capture"]),"clusterCsrExact":cluster_legacy["pointLightStress"]["gridRuntime"]["csrSignature"]==cluster_explicit["pointLightStress"]["gridRuntime"]["csrSignature"]}
    if not all(compat.values()):raise RuntimeError(f"endpoint compatibility {compat}")
    cycle=load(run_dir/ce["cache-slice-cycle"]["result"]);cycle_check={"requested":bool(cycle["pointLightStress"]["gridSliceCycleRequested"]),"buildCount":int(cycle["pointLightStress"]["gridRuntime"]["buildCount"]),"cacheHitCount":int(cycle["pointLightStress"]["gridRuntime"]["cacheHitCount"]),"passed":int(cycle["pointLightStress"]["gridRuntime"]["buildCount"])==31 and int(cycle["pointLightStress"]["gridRuntime"]["cacheHitCount"])==0}
    if not cycle_check["passed"]:raise RuntimeError(f"cache slice switch {cycle_check}")
    high_done=cd["quality-high-n0512-r120-s01"];position=read_pfm(run_dir/high_done["position"]);validity=read_pfm(run_dir/high_done["validity"])>0
    truths=[]
    for key,(result,encoded,tx,ty) in truth_cases.items():truths.append({"lightCount":key[0],"radius":key[1],**truth(position,validity,result,encoded,tx,ty)})
    total_truth=sum(item["groundTruthInteractions"] for item in truths)
    shader=(project/"shaders"/"pointLightGridFragment.glsl").read_text(encoding="utf-8");runtime=(project/"PointLightGridRuntime.cpp").read_text(encoding="utf-8");global_header=(project/"Global.h").read_text(encoding="utf-8")
    shader_checks={"discardBeforeMetadata":shader.index("if (!LoadWorldPosition(texCoords, fragPos)) discard;")<shader.index("texelFetch(gridMetadata"),"sliceUniformControlsDepth":"if (gridSliceCount > 1)" in shader,"exactSpherePredicate":"if (distanceSquared > positionRadius.w) continue;" in shader,"threeTboFetches":all(name in shader for name in ("gridMetadata","gridIndices","gridLights"))}
    source_checks={"sliceInCacheSignature":"HashValue(signature, sliceCount);" in runtime,"sliceControlsLogicalCells":"static_cast<std::uint64_t>(m_stats.sliceCount)" in runtime,"failClosedCapacity":"point-light grid index buffer exceeds capacity" in runtime,"defaultAnalytic":bool(re.search(r"POINT_LIGHT_RENDER_MODE\s*=\s*PointLightRenderProperty::AnalyticScreen",global_header))}
    if not all(shader_checks.values()) or not all(source_checks.values()):raise RuntimeError(f"contract {shader_checks} {source_checks}")
    verification={"schemaVersion":1,"passed":True,"method":"independent NumPy float32 side-plane/log-Z rebuild for every unique N/R/S + compiled streaming FNV + three full G-buffer truths + image gates",
                  "protocolSha256":protocol,"captureManifestSha256":sha(run_dir/"capture-manifest.json"),"correctnessManifestSha256":sha(run_dir/"correctness-manifest.json"),"fnvHelperSourceSha256":sha(Path(__file__).with_name("fnv1a64_stdin.cpp")),"fnvHelperExecutableSha256":sha(helper),
                  "csrCellsVerified":csr_count,"csr":csr,"quality":{"gate":{"maxChannelLsb":2,"meanChannelLsb":.1,"p99ChannelLsb":1},"cases":quality_cases},"edgeCases":edges,"endpointCompatibility":compat,"sliceSwitchCacheInvalidation":cycle_check,
                  "fullImageMembership":{"cases":truths,"totalGroundTruthInteractions":total_truth,"allMissesZero":True},"shaderContract":shader_checks,"sourceContract":source_checks,"defaultRenderModeRemainsAnalyticScreen":source_checks["defaultAnalytic"]}
    dump(run_dir/"verification"/"independent-verification.json",verification)
    print(f"[verification] PASS csr={csr_count} truth={total_truth} misses=0 quality={len(quality_cases)}")
    return 0


if __name__=="__main__":raise SystemExit(main())
