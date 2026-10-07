# Merkzettel: BFS-Panic bei viel Index-Churn

Siehe auch Issue #50.

Beobachtet am 2026-10-06 auf der 64-Bit-VM während eines ProjectConceptor-Release-Builds:

    PANIC: vnode 3:3215341 already exists

Aufrufer: `VolumeWorker` von `index_server`, Pfad
`FullTextAnalyser::LastEntry → CLuceneWriteDataBase::Commit →
IndexWriter::flush → FSIndexOutput` (neue Lucene-Datei).

Vermutung: gleichzeitiger Create/Delete-Churn auf derselben Partition.
Der Server verstärkt das, weil er pro Batch committet. Ungeklärt, ob
BFS-Bug oder seltener Race.

Mögliche Gegenmaßnahmen, noch nicht umgesetzt:
- Commits seltener, Batches zusammenfassen
- Indexierung während großer Builds drosseln
