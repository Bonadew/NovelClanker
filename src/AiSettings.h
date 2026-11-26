#pragma once

#include "LmChat.h"

#include <QString>

// Application-wide AI settings (stored with QSettings, not in the novel).
struct AiSettings {
    static constexpr auto kDefaultServerUrl = "http://127.0.0.1:1234";

    QString serverUrl = kDefaultServerUrl; // LM Studio server address
    QString plannerModel;                  // model id for the orchestrator/planner; empty = not chosen
    QString proseModel;                    // model id for the prose writer; empty = not chosen
    Reasoning reasoning = Reasoning::Default; // how much reasoning models may think before answering

    static AiSettings load();
    void save() const;
};
