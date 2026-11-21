# Plan: checkpoint et reprise de simulation

Statut: phases 0, 1 et 2 **réalisées** (phase 2: voir `CHECKPOINT-DESIGN.md`,
fichiers ouverts, procédures suspendues et access simples compris). Phases 3
et 4 à faire.
Remplace et détaille `PLAN.md` §1.

## 1. Les deux besoins, qui ne se résolvent pas de la même façon

| Besoin | Exemple | Le design change-t-il ? |
|---|---|---|
| **A. Revenir en arrière** | « J'ai oublié d'enregistrer ce signal », « que se passe-t-il si je force ce signal à 180 ms ? », rejouer les 2 ms avant une erreur | Non |
| **B. Reprendre après un correctif** | « Je corrige le bug et je repars de 180 ms au lieu de zéro » | Oui |

A se fait sans rien sérialiser: on duplique le processus du simulateur. B
demande d'extraire l'état, de recompiler, et de réinjecter l'état dans un
design dont le code a changé. A coûte une session; B en coûte plusieurs et
porte une limite de fond (§2).

## 2. La limite de fond du besoin B, à accepter ou non

Quand on reprend à T avec un code corrigé, l'état restauré est celui que
**l'ancien** code a calculé jusqu'à T. Si le correctif change le comportement
avant T, la simulation reprise n'est pas celle qu'on aurait eue en repartant
de zéro.

L'idée de `PLAN.md` (« reprendre au dernier checkpoint antérieur à la
première exécution du code modifié ») ne suffit pas pour du RTL: un process
synchrone s'exécute à chaque front d'horloge depuis le temps zéro, donc le
code modifié a presque toujours déjà tourné.

Ce qu'on peut offrir honnêtement:

- **Reprise exacte** quand le code modifié n'a réellement pas encore été
  exécuté à T. Cela se mesure à la granularité de l'instruction, avec
  l'instrumentation de couverture existante: une branche `when ERREUR =>`
  jamais atteinte, une phase tardive du testbench, un vérificateur. nvc peut
  le prouver et le dire.
- **Reprise approchée** sinon: « comme si le correctif était appliqué à T ».
  Utile pour vérifier vite qu'un correctif traite le symptôme; le résultat
  est marqué comme tel et un run complet reste nécessaire pour valider.

## 3. Ce que le code permet (vérifié)

- La boucle de simulation est **mono-thread** (`src/rt/model.c` n'utilise
  aucune file de travail parallèle). Les seuls threads sont ceux de la
  compilation JIT asynchrone (`OPT_JIT_ASYNC`, `async_barrier()` pour les
  attendre). `fork()` entre deux tranches de simulation est donc praticable.
- Le worker MCP avance déjà par tranches et rend la main entre deux: le
  point d'arrêt cohérent existe.
- Les variables d'un process ont un **nom et un décalage** dans sa trame
  (`linktab`, utilisé par `jit_get_frame_var`): on peut lire et écrire l'état
  d'un process par nom, sans connaître son agencement mémoire.
- Les signaux ont un chemin hiérarchique stable (déjà utilisé par les outils
  MCP).
- La suite de régression (1500 tests) est un oracle tout prêt: sauver à T,
  restaurer, finir, et comparer sorties et ondes au run d'un seul trait.

Ce qui reste à vérifier dans le spike: le tas (`mspace`, types access et
protégés), les fichiers ouverts par le design, l'état de reprise des process
à `wait` multiples, le coût mémoire réel de `fork()`, le build LLVM.

## 4. Phases

Tailles relatives: S = quelques heures, M = une session, L = plusieurs.

### Phase 0. Spike et mesures (M)

But: décider sur des chiffres, avant d'écrire le vrai code.

1. `fork()` du worker à un instant T, puis reprise dans l'enfant: la suite
   est-elle identique au run d'un seul trait (sorties et ondes) ? Sur 5
   designs dont le SoC du banc.
2. Coût: temps du `fork()`, mémoire réellement copiée après reprise, pour
   un design de quelques Mo et un de quelques centaines de Mo.
3. Inventaire de l'état pour la phase 3: ce qui se lit par nom, ce qui
   contient des pointeurs bruts, ce que font les 1500 tests (combien
   utilisent access, protégés, fichiers, VHPI, Verilog).
4. Granularité de « code déjà exécuté »: l'instrumentation de couverture
   donne-t-elle l'instant de première exécution à faible coût ?

**Livrable**: un tableau de mesures et une recommandation go/no-go pour
chaque phase suivante. Je vous le soumets avant de continuer.

### Phase 1. Retour en arrière par instantanés (besoin A) (M)

Dans le serveur MCP, sans format de fichier.

- **`nvc_sim_checkpoint`**: fige une copie du worker à l'instant courant
  (un processus gelé, pages partagées en copie à l'écriture). Rend un
  identifiant, le temps simulé, la mémoire.
- **`nvc_sim_restore`**: crée une **nouvelle simulation** (nouveau `sim_id`)
  qui repart du checkpoint. L'originale continue ou est fermée, au choix:
  on peut donc explorer deux suites à partir du même instant.
- **Checkpoints automatiques** pendant un run de fond: espacement en temps
  réel avec éclaircissage exponentiel (récents denses, anciens rares),
  nombre et mémoire bornés, surcoût visé inférieur à 2 %. `nvc_sim_status`
  les liste.
- **Fichiers**: la simulation restaurée a son propre répertoire; le log est
  copié jusqu'au point de reprise; un enregistrement FST repart dans un
  nouveau fichier à T; l'enregistrement en mémoire est copié tel quel.
- **Limites affichées**: même design et même binaire; perdu si le serveur
  s'arrête; indisponible avec VHPI/VPI ou hors Linux/macOS.

Usage visé: un run de fond de plusieurs heures échoue à 180 ms; l'agent
restaure le checkpoint à 178 ms, démarre l'enregistrement des signaux
utiles, relance 2 ms. Quelques secondes au lieu de plusieurs heures.

### Phase 2. Sauvegarde et restauration typées, même design (L)

Le socle du besoin B, et de la persistance sur disque.

- **Format**: par chemin de signal (valeurs, pilotes, transactions en
  attente), par process (variables par nom, point de reprise), ordonnanceur
  (temps, delta, files), plus une empreinte du design.
- **Périmètre initial volontairement étroit**: VHDL, sans access ni types
  protégés ni fichiers ouverts ni VHPI. Hors périmètre: refus explicite avec
  la raison, jamais de reprise silencieusement fausse.
- **Validation**: pour chaque test de régression dans le périmètre, sauver
  à mi-course, restaurer dans un processus neuf, comparer au run d'un seul
  trait. Objectif: 100 % d'identité sur les tests acceptés, et le taux de
  tests acceptés comme mesure d'avancement.
- **Interface**: `nvc -r --checkpoint=FICHIER@TEMPS` et
  `nvc -r --restore=FICHIER`, plus les mêmes outils MCP que la phase 1
  avec persistance.

### Phase 3. Reprise après correctif (besoin B) (L)

- **Empreinte par unité** (process, sous-programme): inchangée → tout est
  restauré, point de reprise compris.
- **Unité modifiée**: variables restaurées par nom quand le type n'a pas
  changé, nouvelles variables à leur valeur initiale. Point de reprise
  restauré seulement si le process a une liste de sensibilité (il repart
  toujours du début); sinon refus de cette reprise, avec l'explication.
- **Signaux**: par chemin; ajoutés → valeur initiale; supprimés → ignorés;
  type changé → refus.
- **Verdict affiché à chaque reprise**: *exacte* (le code modifié n'avait
  pas tourné avant T, prouvé) ou *approchée* (liste des unités modifiées
  déjà exécutées). Jamais l'un présenté comme l'autre.
- **Mode invisible**: checkpoints automatiques dans un cache à côté de la
  bibliothèque; au run suivant nvc propose le dernier point de reprise
  exact, ou approché si demandé. `--no-resume` pour repartir de zéro.

### Phase 4. Élargir le périmètre (L, par morceaux)

Dans l'ordre que dicteront les mesures de la phase 0: fichiers ouverts
(nom et position), tas (access, tableaux dynamiques), types protégés,
couverture, SystemVerilog, ondes en continu.

## 5. Ordre et points d'arrêt

0 → 1 → (décision) → 2 → 3 → 4.

Je m'arrête et vous rends compte après la phase 0 (mesures) et après la
phase 1 (premier usage réel), avant d'engager les phases longues.

## 6. Risques

- **Reprise fausse sans le savoir.** Parade: refus explicite hors périmètre,
  empreinte du design, comparaison systématique sur la régression.
- **Mémoire des instantanés** sur de gros designs. Parade: budget, mesure en
  phase 0, éclaircissage.
- **Périmètre trop étroit pour vos designs** (textio, access dans les
  testbenchs). Parade: l'inventaire de la phase 0 sur vos cas d'usage; vos
  designs réels seraient la meilleure mesure.
- **Coût de la phase 3** pour un bénéfice limité par §2. Parade: décision
  après la phase 1, quand on saura ce que le retour en arrière couvre déjà.
- **Build LLVM**: non testable dans cet environnement (LLVM cassé ici);
  les threads de compilation asynchrone devront être attendus avant un
  `fork()`.

## 7. Décisions à prendre

1. **Accepter la limite du §2**: une reprise après correctif est exacte
   seulement si le code modifié n'avait pas encore tourné, approchée sinon.
2. **Commencer par le retour en arrière (phases 0 et 1)** avant la reprise
   après correctif (recommandé): gain rapide, et il dit ce qui reste
   vraiment à couvrir.
3. **Périmètre initial de la phase 2**: VHDL sans access, protégés,
   fichiers ni VHPI. Vos testbenchs utilisent-ils textio ou des fichiers de
   stimuli ? Si oui, les fichiers passent en tête de la phase 4.
4. **Interface**: outils MCP d'abord (phase 1), ligne de commande à partir
   de la phase 2.
