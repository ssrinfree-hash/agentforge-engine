#!/usr/bin/env python
"""AgentForge — Kaggle-ядро ассет-интеллекта.

1. Скачивает ModelNet40 (12 311 OFF-моделей, 40 классов) через kagglehub.
2. Сканирует все модели → каталог catalog.jsonl (id, class, verts, faces, bbox).
3. Обучает модель поиска: hash-ngram(текст запроса) → класс ассета (40-way softmax).
   Устройство: TPU (torch_xla) → GPU → CPU autodetect (Agnes-ревью требование).
4. Оценивает: name→class accuracy на holdout, MRR@5 на синтетических запросах.
5. Экспортирует: asset_model.bin (формат AFASSET1 для C++ движка),
   catalog.jsonl, model_card.md, eval.json, dataset_stats.json.

Выход: /kaggle/working/{catalog.jsonl, asset_model.bin, model_card.md, eval.json, ...}
"""
import hashlib
import json
import math
import os
import random
import sys
import time
from pathlib import Path

WORK = Path("/kaggle/working") if Path("/kaggle/working").exists() else Path.cwd()
LOG = WORK / "asset_kernel_log.txt"

VOCAB = 4096
CLASSES_EXPECTED = 40
HOLDOUT_FRACTION = 0.15
SEED = 42


def log(msg):
    line = f"[{time.strftime('%H:%M:%S')}] {msg}"
    print(line, flush=True)
    with open(LOG, "a") as f:
        f.write(line + "\n")


def pick_device():
    """TPU → GPU → CPU (требование ревью: явный autodetect + inference_mode)."""
    try:
        import torch_xla.core.xla_model as xm
        dev = xm.xla_device()
        log("device: TPU (torch_xla)")
        return "xla", dev
    except Exception:
        pass
    try:
        import torch
        if torch.cuda.is_available():
            log(f"device: GPU ({torch.cuda.get_device_name(0)})")
            return "cuda", torch.device("cuda")
    except Exception:
        pass
    import torch
    log("device: CPU")
    return "cpu", torch.device("cpu")


def hash_feats(text, vocab=VOCAB):
    """Слова + символьные 3-граммы → L2-нормированный hashed bag-of-words."""
    feats = [0.0] * vocab
    low = text.lower()

    def h(t):
        return int(hashlib.md5(t.encode()).hexdigest(), 16) % vocab

    word = ""
    for i, c in enumerate(low + " "):
        if c.isalnum():
            word += c
        elif word:
            feats[h(word)] += 1.0
            word = ""
    for i in range(len(low) - 2):
        if low[i] != " ":
            feats[h(low[i:i + 3])] += 1.0
    norm = math.sqrt(sum(v * v for v in feats)) or 1.0
    return [v / norm for v in feats]


def scan_dataset(root):
    """ModelNet40: <root>/modelnet40_normal_resampled/<class>/<id>.off (или .txt)."""
    entries = []
    off_files = sorted(root.rglob("*.off"))
    log(f"found {len(off_files)} OFF files")
    for i, p in enumerate(off_files):
        # структура ModelNet40/<class>/{train,test}/<id>.off
        cls = p.parent.name
        if cls in ("train", "test"):
            cls = p.parent.parent.name
        if cls in ("train", "test", "ModelNet40"):
            cls = p.parent.parent.parent.name if len(p.parts) > 3 else cls
        verts = faces = 0
        mn = [1e9] * 3
        mx = [-1e9] * 3
        try:
            with open(p, "r", errors="ignore") as f:
                header = f.readline()
                if not header.startswith(("OFF", "COFF")):
                    continue
                nv, nf, _ = header.split()[1:4] if len(header.split()) >= 3 else (0, 0, 0)
                nv, nf = int(nv), int(nf)
                verts_read = 0
                for line in f:
                    parts = line.split()
                    if not parts:
                        continue
                    if verts_read < nv:
                        if len(parts) >= 3:
                            x, y, z = float(parts[0]), float(parts[1]), float(parts[2])
                            for k, v in enumerate((x, y, z)):
                                mn[k] = min(mn[k], v)
                                mx[k] = max(mx[k], v)
                            verts_read += 1
                        continue
                    if parts[0].isdigit() and len(parts) >= 4:
                        faces += 1
            verts = verts_read
        except Exception as e:  # noqa: BLE001
            log(f"skip {p.name}: {e}")
            continue
        if verts == 0:
            continue
        entries.append({
            "id": p.stem,
            "class": cls,
            "name": cls.replace("_", " "),
            "verts": verts,
            "faces": faces,
            "bbox": [round(mn[0], 4), round(mn[1], 4), round(mn[2], 4),
                     round(mx[0], 4), round(mx[1], 4), round(mx[2], 4)],
        })
        if (i + 1) % 1000 == 0:
            log(f"scanned {i + 1}/{len(off_files)}")
    return entries


def main():
    t0 = time.time()
    log("AgentForge asset-intelligence kernel start")
    random.seed(SEED)

    # Датасет подключён через kernel-metadata dataset_sources (batch-сессии
    # не могут качать новые датасеты через kagglehub — ошибка code 9).
    found = list(Path("/kaggle/input").rglob("*.off")) if Path("/kaggle/input").exists() else []
    if found:
        data_root = found[0].parent.parent.parent  # .../ModelNet40/<class>/<split>/file.off
        log(f"dataset mounted at /kaggle/input: {len(found)} OFF files")
    else:
        raise RuntimeError(
            "ModelNet40 не подключён к ядру — добавь "
            "balraj98/modelnet40-princeton-3d-object-dataset в dataset_sources")

    entries = scan_dataset(data_root)
    classes = sorted({e["class"] for e in entries})
    log(f"catalog: {len(entries)} models, {len(classes)} classes")
    if len(entries) < 100:
        raise RuntimeError(f"too few models scanned: {len(entries)}")

    with open(WORK / "catalog.jsonl", "w") as f:
        for e in entries:
            f.write(json.dumps(e) + "\n")

    # ── Обучение: hashed features → класс (linear softmax) ──
    import torch
    import torch.nn as nn
    kind, device = pick_device()

    random.shuffle(entries)
    n_hold = int(len(entries) * HOLDOUT_FRACTION)
    test_set, train_set = entries[:n_hold], entries[n_hold:]
    cls_index = {c: i for i, c in enumerate(classes)}
    cls_names = classes  # индекс = порядок в sorted list

    Xtr = torch.tensor([hash_feats(e["id"] + " " + e["name"]) for e in train_set])
    ytr = torch.tensor([cls_index[e["class"]] for e in train_set])
    Xte = torch.tensor([hash_feats(e["id"] + " " + e["name"]) for e in test_set])
    yte = torch.tensor([cls_index[e["class"]] for e in test_set])

    W = nn.Linear(VOCAB, len(classes))
    W.to(device)
    opt = torch.optim.Adam(W.parameters(), lr=0.05)
    lossf = nn.CrossEntropyLoss()
    EPOCHS = 40
    with torch.no_grad():
        pass
    Xtr_d = Xtr.to(device)
    ytr_d = ytr.to(device)
    for ep in range(EPOCHS):
        opt.zero_grad()
        out = W(Xtr_d)
        loss = lossf(out, ytr_d)
        loss.backward()
        opt.step()
        if kind == "xla":
            import torch_xla.core.xla_model as xm
            xm.mark_step()
        if (ep + 1) % 10 == 0:
            log(f"epoch {ep + 1}/{EPOCHS} loss={loss.item():.4f}")

    # ── Оценка ──
    with torch.no_grad():
        W.eval()
        logits = W(Xte.to(device))
        if kind == "xla":
            import torch_xla.core.xla_model as xm
            xm.mark_step()
        pred = logits.argmax(dim=1).cpu()
        acc = (pred == yte).float().mean().item()
        # MRR@5 по запросам-синонимам
        queries = {
            "chair": "chair", "wooden chair": "chair", "office chair": "chair",
            "sofa": "sofa", "couch": "sofa", "table": "table", "desk": "table",
            "lamp": "lamp", "night lamp": "lamp", "bed": "bed",
            "airplane": "airplane", "plane": "airplane", "car": "car",
            "automobile": "car", "guitar": "guitar", "vase": "vase",
            "cup": "cup", "mug": "cup", "bottle": "bottle",
            "laptop": "laptop", "computer notebook": "laptop",
            "monitor": "monitor", "tv screen": "monitor",
            "piano": "piano", "keyboard instrument": "piano",
            "bench": "bench", "stool seat": "bench", "tent": "tent",
            "wardrobe": "wardrobe", "dresser": "dresser", "night stand": "night_stand",
            "toilet": "toilet", "sink": "sink", "bathtub": "bathtub",
            "door": "door", "curtain": "curtain", "flower pot": "flower_pot",
            "person": "person", "bookcase": "bookcase", "swivel chair seat": "chair",
        }
        with torch.no_grad():
            Wq = torch.tensor(
                [hash_feats(q) for q in queries], device=device)
            qlog = W(Wq)
            if kind == "xla":
                import torch_xla.core.xla_model as xm
                xm.mark_step()
            probs = torch.softmax(qlog, dim=1).cpu()
        mrrs = []
        for qi, (q, want) in enumerate(queries.items()):
            ranked = torch.argsort(probs[qi], descending=True)[:5]
            rr = 0.0
            for rank, ci in enumerate(ranked.tolist()):
                if cls_names[ci] == want:
                    rr = 1.0 / (rank + 1)
                    break
            mrrs.append(rr)
        mrr = sum(mrrs) / len(mrrs)
        log(f"holdout name→class acc: {acc:.4f}")
        log(f"synonym-query MRR@5: {mrr:.4f}")

    # ── Экспорт (формат AFASSET1 для C++ AssetSearchModel) ──
    Wm = W.weight.detach().cpu().numpy()          # [classes, vocab]
    import struct
    with open(WORK / "asset_model.bin", "wb") as f:
        f.write(b"AFASSET1")
        f.write(struct.pack("<4i", VOCAB, len(classes), VOCAB, len(classes)))
        # движок ждёт W как [vocab x classes] row-major
        f.write(Wm.T.astype("<f4").tobytes())
        for name in cls_names:
            b = name.encode()
            f.write(struct.pack("<i", len(b)))
            f.write(b)
    log("exported asset_model.bin")

    stats = {
        "models_total": len(entries),
        "classes": len(classes),
        "class_names": classes,
        "verts_sum": sum(e["verts"] for e in entries),
        "faces_sum": sum(e["faces"] for e in entries),
        "holdout_accuracy": round(acc, 4),
        "synonym_mrr5": round(mrr, 4),
        "vocab": VOCAB,
        "device": kind,
        "runtime_s": round(time.time() - t0, 1),
    }
    (WORK / "dataset_stats.json").write_text(json.dumps(stats, indent=2, ensure_ascii=False))
    (WORK / "eval.json").write_text(json.dumps(
        {"holdout_accuracy": acc, "synonym_mrr5": mrr,
         "holdout_size": len(test_set), "queries": len(queries)}, indent=2))
    (WORK / "model_card.md").write_text(
        "# AgentForge Asset Intelligence — ModelNet40\n\n"
        f"- Моделей в каталоге: **{len(entries)}** (12 311 в полном ModelNet40), классов: {len(classes)}\n"
        f"- Модель: hash-ngram ({VOCAB}) → linear softmax ({len(classes)} классов)\n"
        f"- Device: {kind}\n"
        f"- Holdout accuracy (имя файла → класс): **{acc:.4f}**\n"
        f"- MRR@5 на синоним-запросах: **{mrr:.4f}**\n"
        "- Формат весов: AFASSET1 (asset_model.bin), читается C++ AssetSearchModel.\n"
        "- Использование в движке: `afengine serve --catalog catalog.jsonl --weights asset_model.bin`, "
        "запрос `{\"cmd\":\"asset.search\",\"query\":\"chair\",\"top_k\":10}`.\n")
    log("DONE")


if __name__ == "__main__":
    main()
