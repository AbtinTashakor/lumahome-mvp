#pragma once

class LightController {
public:
    void begin();
    void setLight(bool on);

private:
    bool lightOn_ = false;
};
