# Plan: simulation incrémentale, enregistrement a posteriori et serveur MCP

Document de réflexion (aucun code écrit à ce stade). Il regroupe les idées
discutées pour améliorer l'utilisation de nvc sur de longues simulations.

## Objectifs

1. Reprendre une simulation longue juste avant un bug, après correction du code.
2. Éviter de re-simuler les phases lentes (amorçage par liaison série, etc.).
3. Ne plus avoir à deviner à l'avance quels signaux enregistrer.
4. Rendre nvc facilement pilotable par un LLM (serveur MCP natif).

Contrainte: les solutions doivent être faites dans nvc et rester le plus
possible invisibles pour l'utilisateur.

## 1. Checkpoint / simulation incrémentale (priorité 1)

### État à sauvegarder (fin de delta cycle)
- Signaux: valeurs effectives et pilotées (nexus), sources, drivers,
  waveforms en attente.
- Ordonnanceur: `now`, delta, itération, `eventq_heap`, `driving_heap`,
  `effective_heap`, wakeables (processus, triggers).
- Processus: `privdata` (frames), `tlab` des processus suspendus,
  `privdata` des scopes.
- Tas `mspace` (access, types protégés, tableaux dynamiques).
- RNG, fichiers ouverts (nom, position), couverture.

### Difficulté principale
Les frames, le `mspace` (GC conservatif) et les `rt_signal_t` contiennent des
pointeurs bruts. Options:

| Approche | Survit à un correctif | Coût | Remarque |
|---|---|---|---|
| `fork()` / CRIU | Non | Faible | Retour arrière seulement, même binaire |
| Dump brut, arènes à adresses fixes | Oui si layout identique | Moyen | Pointeurs de code/ASLR à vérifier |
| Sérialisation typée | Oui | Élevé | Robuste; `lower` émet un descripteur de layout par frame |

### Mécanisme invisible: reprise sûre par construction
- Checkpoints automatiques pendant `nvc -r` (espacement exponentiel, taille
  bornée) dans un cache à côté de la bibliothèque `work`.
- Hash du code compilé de chaque unité (fonction, corps de processus) et temps
  simulé de sa première exécution (le JIT est paresseux par nom de fonction,
  `jit_lazy_compile`, donc le hook est quasi gratuit).
- Au run suivant: reprise au dernier checkpoint antérieur à la première
  exécution d'une unité modifiée. La simulation est déterministe, donc l'état
  est identique tant que le code exécuté est identique.
- Entrées non déterministes dans la clé du cache: fichiers lus (hash et temps
  de première lecture), generics `-g`, `--seed`, `getenv`. Plugins
  VHPI/VPI/foreign: reprise désactivée.
- Sorties passées (stdout, `report`) stockées et rejouées; ondes: VCD par
  ajout, FST via nouveau fichier ou copie du préfixe.
- Option `--no-resume` pour forcer un démarrage à zéro; message d'information
  sur le point de reprise.

### Étapes
1. **Spike** (quelques jours): arènes à adresses fixes, comptage des pointeurs
   sortant des arènes, mesure de taille et temps de sauvegarde.
2. Squelette: sauvegarde/restauration à la fin du delta cycle, format de
   fichier, hash de compatibilité du design. Refus explicite pour access,
   protected et fichiers ouverts au début.
3. Tas: graphe des access, protected, fichiers.
4. SystemVerilog: `$random`, `$fopen`, processus Verilog.
5. Sorties: ondes et couverture.

## 2. Période calme (priorité 3, gain limité)

- nvc filtre déjà les réveils sur front d'horloge via les triggers
  (`FUNC_TRIGGER`, `CMP_TRIGGER`, `LEVEL_TRIGGER` dans `model.c`).
- Un compteur qui compte n'est pas calme: pas de saut possible sans
  accélération d'induction (non généralisable).
- Réalisable et sûr: saut quand tous les processus réveillés sont des no-op
  prouvés (suivi dynamique lectures/écritures dans le JIT, processus impurs
  exclus), et détection de cycle d'état par hash (réutilise l'état du
  checkpoint).
- Ne réduit pas le coût d'un transfert série en cours.

## 3. Enregistrement des signaux

Situation actuelle: `--wave`, `--format=fst|vcd`, `--include`, `--exclude`,
`--dump-arrays`; le choix se fait avant le run.

1. Réduire le coût de « tout enregistrer » (tampon par thread, compression en
   arrière-plan, pas de valeur dupliquée). **À mesurer d'abord** sur un design
   réel.
2. Ajout a posteriori: `--wave-add=GLOB`, reprise au dernier checkpoint, rejeu
   de la fenêtre voulue (exact car déterministe).
3. Enregistrement léger permanent (ports, contrôle) plus détail à la demande.
4. Fenêtre temporelle: `--wave-window=T1..T2`.

Les points 2 à 4 dépendent du checkpoint.

## 4. Serveur MCP natif (priorité 2)

Architecture détaillée et décisions: voir `MCP-ARCHITECTURE.md`.

Mode `nvc --mcp`: JSON-RPC 2.0 sur stdio, module `src/mcp/` (~1000-1500
lignes), parseur JSON minimal à ajouter (aucun JSON dans le code actuel).
S'appuie sur le shell TCL (`shell_eval`) et l'API `model_*`.

Outils: `analyse`, `elaborate`, `run`, `examine`, `force`/`release`,
`describe`/`list_scopes`, `get_waveform`, `run_test`, `coverage_report`.

Points de conception:
- Sorties en JSON structuré et concis (diagnostics: fichier, ligne,
  sévérité, message), listes et ondes tronquées avec indicateur.
- Session persistante; avec checkpoints, un agent peut revenir en arrière,
  corriger et reprendre.
- Mode restreint par défaut (pas de `exec` TCL, pas de plugin, chemins limités
  au répertoire de travail).
- Compilation optionnelle (`--enable-mcp`) ou sans dépendance externe.
- Version minimale: `analyse`, `run`, `examine`.

## 5. Accélération par GPU (écartée pour l'instant)

Mauvais candidat pour un simulateur événementiel (synchronisation à chaque
delta). Pistes notées pour mémoire: fonctions pures déportées, simulation par
lots, backend cycle-based pour pipeline pixel, gate-level. Pour la vidéo,
modèle de référence externe et parallélisme au niveau image; pour les liaisons
série, générique de débit, modèle fonctionnel, chargement par backdoor.

## Ordre proposé

1. Mesures: surcoût de `--wave` sans filtre, profil d'une simulation type.
2. Spike checkpoint (arènes fixes, pointeurs hors arène).
3. Checkpoint incrémental invisible.
4. MCP minimal, puis extension; ajout a posteriori de signaux.
5. Période calme (saut de silence, détection de cycle) si les mesures le
   justifient.

## Prochaine étape

Comparer ce fork avec le dépôt nvc amont (retard éventuel) avant de commencer
tout développement.
